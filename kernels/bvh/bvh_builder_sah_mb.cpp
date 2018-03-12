// ======================================================================== //
// Copyright 2009-2018 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh.h"
#include "bvh_builder.h"
#include "../builders/bvh_builder_msmblur.h"

#include "../builders/primrefgen.h"
#include "../builders/splitter.h"

#include "../geometry/linei.h"
#include "../geometry/triangle.h"
#include "../geometry/trianglev.h"
#include "../geometry/trianglev_mb.h"
#include "../geometry/trianglei.h"
#include "../geometry/quadv.h"
#include "../geometry/quadi.h"
#include "../geometry/object.h"
#include "../geometry/instance.h"
#include "../geometry/subgrid.h"

#include "../common/state.h"

// FIXME: remove after removing BVHNBuilderMBlurRootTimeSplitsSAH
#include "../../common/algorithms/parallel_for_for.h"
#include "../../common/algorithms/parallel_for_for_prefix_sum.h"


namespace embree
{
  namespace isa
  {

    template<int N, typename Primitive>
    struct CreateMBlurLeaf
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::NodeRecordMB NodeRecordMB;

      __forceinline CreateMBlurLeaf (BVH* bvh, PrimRef* prims, size_t time) : bvh(bvh), prims(prims), time(time) {}

      __forceinline NodeRecordMB operator() (const PrimRef* prims, const range<size_t>& set, const FastAllocator::CachedAllocator& alloc) const
      {
        size_t items = Primitive::blocks(set.size());
        size_t start = set.begin();
        Primitive* accel = (Primitive*) alloc.malloc1(items*sizeof(Primitive),BVH::byteAlignment);
        NodeRef node = bvh->encodeLeaf((char*)accel,items);

        LBBox3fa allBounds = empty;
        for (size_t i=0; i<items; i++)
          allBounds.extend(accel[i].fillMB(prims, start, set.end(), bvh->scene, time));

        return NodeRecordMB(node,allBounds);
      }

      BVH* bvh;
      PrimRef* prims;
      size_t time;
    };


    template<int N, typename Mesh, typename Primitive>
    struct CreateMSMBlurLeaf
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::NodeRecordMB4D NodeRecordMB4D;

      __forceinline CreateMSMBlurLeaf (BVH* bvh) : bvh(bvh) {}

      __forceinline const NodeRecordMB4D operator() (const BVHBuilderMSMBlur::BuildRecord& current, const FastAllocator::CachedAllocator& alloc) const
      {
        size_t items = Primitive::blocks(current.prims.object_range.size());
        size_t start = current.prims.object_range.begin();
        Primitive* accel = (Primitive*) alloc.malloc1(items*sizeof(Primitive),BVH::byteNodeAlignment);
        NodeRef node = bvh->encodeLeaf((char*)accel,items);
        LBBox3fa allBounds = empty;
        for (size_t i=0; i<items; i++)
          allBounds.extend(accel[i].fillMB(current.prims.prims->data(), start, current.prims.object_range.end(), bvh->scene, current.prims.time_range));
        return NodeRecordMB4D(node,allBounds,current.prims.time_range);
      }

      BVH* bvh;
    };

    /* Motion blur BVH with 4D nodes and internal time splits */
    template<int N, typename Mesh, typename Primitive>
    struct BVHNBuilderMBlurSAH : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      typedef typename BVHN<N>::NodeRecordMB NodeRecordMB;
      typedef typename BVHN<N>::AlignedNodeMB AlignedNodeMB;

      BVH* bvh;
      Scene* scene;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;

      BVHNBuilderMBlurSAH (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize)
        : bvh(bvh), scene(scene), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(min(maxLeafSize,Primitive::max_size()*BVH::maxLeafBlocks)) {}

      void build()
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<Mesh,true>();
        if (numPrimitives == 0) { bvh->clear(); return; }

        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMBlurSAH");

#if PROFILE
        profile(2,PROFILE_RUNS,numPrimitives,[&] (ProfileTimer& timer) {
#endif

            const size_t numTimeSteps = scene->getNumTimeSteps<Mesh,true>();
            const size_t numTimeSegments = numTimeSteps-1; assert(numTimeSteps > 1);

            if (numTimeSegments == 1)
              buildSingleSegment(numPrimitives);
            else
              buildMultiSegment(numPrimitives);

#if PROFILE
          });
#endif

	/* clear temporary data for static geometry */
        if (scene->isStaticAccel()) bvh->shrink();
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void buildSingleSegment(size_t numPrimitives)
      {
        /* create primref array */
        mvector<PrimRef> prims(scene->device,numPrimitives);
        const PrimInfo pinfo = createPrimRefArrayMBlur(scene,Mesh::geom_type,prims,bvh->scene->progressInterface,0);

        /* estimate acceleration structure size */
        const size_t node_bytes = pinfo.size()*sizeof(AlignedNodeMB)/(4*N);
        const size_t leaf_bytes = size_t(1.2*Primitive::blocks(pinfo.size())*sizeof(Primitive));
        bvh->alloc.init_estimate(node_bytes+leaf_bytes);

        /* settings for BVH build */
        GeneralBVHBuilder::Settings settings;
        settings.branchingFactor = N;
        settings.maxDepth = BVH::maxBuildDepthLeaf;
        settings.logBlockSize = bsr(sahBlockSize);
        settings.minLeafSize = minLeafSize;
        settings.maxLeafSize = maxLeafSize;
        settings.travCost = travCost;
        settings.intCost = intCost;
        settings.singleThreadThreshold = bvh->alloc.fixSingleThreadThreshold(N,DEFAULT_SINGLE_THREAD_THRESHOLD,pinfo.size(),node_bytes+leaf_bytes);

        /* build hierarchy */
        auto root = BVHBuilderBinnedSAH::build<NodeRecordMB>
          (typename BVH::CreateAlloc(bvh),typename BVH::AlignedNodeMB::Create2(),typename BVH::AlignedNodeMB::Set2(),
           CreateMBlurLeaf<N,Primitive>(bvh,prims.data(),0),bvh->scene->progressInterface,
           prims.data(),pinfo,settings);

        bvh->set(root.ref,root.lbounds,pinfo.size());
      }

      void buildMultiSegment(size_t numPrimitives)
      {
        /* create primref array */
        mvector<PrimRefMB> prims(scene->device,numPrimitives);
        PrimInfoMB pinfo = createPrimRefArrayMSMBlur(scene,Mesh::geom_type,prims,bvh->scene->progressInterface);

        /* estimate acceleration structure size */
        const size_t node_bytes = pinfo.num_time_segments*sizeof(AlignedNodeMB)/(4*N);
        const size_t leaf_bytes = size_t(1.2*Primitive::blocks(pinfo.num_time_segments)*sizeof(Primitive));
        bvh->alloc.init_estimate(node_bytes+leaf_bytes);

        /* settings for BVH build */
        BVHBuilderMSMBlur::Settings settings;
        settings.branchingFactor = N;
        settings.maxDepth = BVH::maxDepth;
        settings.logBlockSize = bsr(sahBlockSize);
        settings.minLeafSize = minLeafSize;
        settings.maxLeafSize = maxLeafSize;
        settings.travCost = travCost;
        settings.intCost = intCost;
        settings.singleLeafTimeSegment = Primitive::singleTimeSegment;
        settings.singleThreadThreshold = bvh->alloc.fixSingleThreadThreshold(N,DEFAULT_SINGLE_THREAD_THRESHOLD,pinfo.size(),node_bytes+leaf_bytes);
        
        /* build hierarchy */
        auto root =
          BVHBuilderMSMBlur::build<NodeRef>(prims,pinfo,scene->device,
                                            RecalculatePrimRef<Mesh>(scene),
                                            typename BVH::CreateAlloc(bvh),
                                            typename BVH::AlignedNodeMB4D::Create(),
                                            typename BVH::AlignedNodeMB4D::Set(),
                                            CreateMSMBlurLeaf<N,Mesh,Primitive>(bvh),
                                            bvh->scene->progressInterface,
                                            settings);

        bvh->set(root.ref,root.lbounds,pinfo.num_time_segments);
      }

      void clear() {
      }
    };

    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/


    template<int N>
    struct CreateMBlurLeafGrid
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::NodeRecordMB NodeRecordMB;

      __forceinline CreateMBlurLeafGrid (BVH* bvh, PrimRef* prims, size_t time) : bvh(bvh), prims(prims), time(time) {}

      __forceinline NodeRecordMB operator() (const PrimRef* prims, const range<size_t>& set, const FastAllocator::CachedAllocator& alloc) const
      {
        // size_t items = Primitive::blocks(set.size());
        // size_t start = set.begin();
        // Primitive* accel = (Primitive*) alloc.malloc1(items*sizeof(Primitive),BVH::byteAlignment);
        // NodeRef node = bvh->encodeLeaf((char*)accel,items);

        // LBBox3fa allBounds = empty;
        // for (size_t i=0; i<items; i++)
        //   allBounds.extend(accel[i].fillMB(prims, start, set.end(), bvh->scene, time));

        // return NodeRecordMB(node,allBounds);
        return NodeRecordMB();
      }

      BVH* bvh;
      PrimRef* prims;
      size_t time;
    };


    template<int N>
    struct CreateMSMBlurLeafGrid
    {
      typedef BVHN<N> BVH;
      typedef typename BVH::NodeRef NodeRef;
      typedef typename BVH::NodeRecordMB4D NodeRecordMB4D;

      __forceinline CreateMSMBlurLeafGrid (BVH* bvh) : bvh(bvh) {}

      __forceinline const NodeRecordMB4D operator() (const BVHBuilderMSMBlur::BuildRecord& current, const FastAllocator::CachedAllocator& alloc) const
      {
        // size_t items = Primitive::blocks(current.prims.object_range.size());
        // size_t start = current.prims.object_range.begin();
        // Primitive* accel = (Primitive*) alloc.malloc1(items*sizeof(Primitive),BVH::byteNodeAlignment);
        // NodeRef node = bvh->encodeLeaf((char*)accel,items);
        // LBBox3fa allBounds = empty;
        // for (size_t i=0; i<items; i++)
        //   allBounds.extend(accel[i].fillMB(current.prims.prims->data(), start, current.prims.object_range.end(), bvh->scene, current.prims.time_range));
        // return NodeRecordMB4D(node,allBounds,current.prims.time_range);
        return NodeRecordMB4D();
      }

      BVH* bvh;
    };


    /* Motion blur BVH with 4D nodes and internal time splits */
    template<int N>
    struct BVHNBuilderMBlurSAHGrid : public Builder
    {
      typedef BVHN<N> BVH;
      typedef typename BVHN<N>::NodeRef NodeRef;
      typedef typename BVHN<N>::NodeRecordMB NodeRecordMB;
      typedef typename BVHN<N>::AlignedNodeMB AlignedNodeMB;

      BVH* bvh;
      Scene* scene;
      const size_t sahBlockSize;
      const float intCost;
      const size_t minLeafSize;
      const size_t maxLeafSize;

      BVHNBuilderMBlurSAHGrid (BVH* bvh, Scene* scene, const size_t sahBlockSize, const float intCost, const size_t minLeafSize, const size_t maxLeafSize)
        : bvh(bvh), scene(scene), sahBlockSize(sahBlockSize), intCost(intCost), minLeafSize(minLeafSize), maxLeafSize(maxLeafSize) {} //todo check max leaf size 


      PrimInfo createPrimRefArrayMBlurGrid(Scene* scene, mvector<PrimRef>& prims, BuildProgressMonitor& progressMonitor, size_t itime)
      {
        return PrimInfo(empty);
      }

      PrimInfoMB createPrimRefArrayMSMBlurGrid(Scene* scene, mvector<PrimRefMB>& prims, BuildProgressMonitor& progressMonitor, BBox1f t0t1 = BBox1f(0.0f,1.0f))
      {
        return PrimInfoMB(empty);        
      }

      void build()
      {
	/* skip build for empty scene */
        const size_t numPrimitives = scene->getNumPrimitives<GridMesh,true>();
        if (numPrimitives == 0) { bvh->clear(); return; }

        double t0 = bvh->preBuild(TOSTRING(isa) "::BVH" + toString(N) + "BuilderMBlurSAHGrid");

        const size_t numTimeSteps = scene->getNumTimeSteps<GridMesh,true>();
        const size_t numTimeSegments = numTimeSteps-1; assert(numTimeSteps > 1);

        if (numTimeSegments == 1)
          buildSingleSegment(numPrimitives);
        else
          buildMultiSegment(numPrimitives);

	/* clear temporary data for static geometry */
        if (scene->isStaticAccel()) bvh->shrink();
	bvh->cleanup();
        bvh->postBuild(t0);
      }

      void buildSingleSegment(size_t numPrimitives)
      {
        /* create primref array */
        mvector<PrimRef> prims(scene->device,numPrimitives);
        const PrimInfo pinfo = createPrimRefArrayMBlurGrid(scene,prims,bvh->scene->progressInterface,0);

        /* estimate acceleration structure size */
        const size_t node_bytes = pinfo.size()*sizeof(AlignedNodeMB)/(4*N);
        //const size_t leaf_bytes = size_t(1.2*Primitive::blocks(pinfo.size())*sizeof(SubGridQBVHN<N>));
        //TODO: check leaf_bytes
        const size_t leaf_bytes = size_t(1.2*(float)numPrimitives/N * sizeof(SubGridQBVHN<N>));
        bvh->alloc.init_estimate(node_bytes+leaf_bytes);

        /* settings for BVH build */
        GeneralBVHBuilder::Settings settings;
        settings.branchingFactor = N;
        settings.maxDepth = BVH::maxBuildDepthLeaf;
        settings.logBlockSize = bsr(sahBlockSize);
        settings.minLeafSize = minLeafSize;
        settings.maxLeafSize = maxLeafSize;
        settings.travCost = travCost;
        settings.intCost = intCost;
        settings.singleThreadThreshold = bvh->alloc.fixSingleThreadThreshold(N,DEFAULT_SINGLE_THREAD_THRESHOLD,pinfo.size(),node_bytes+leaf_bytes);

        /* build hierarchy */
        auto root = BVHBuilderBinnedSAH::build<NodeRecordMB>
          (typename BVH::CreateAlloc(bvh),typename BVH::AlignedNodeMB::Create2(),typename BVH::AlignedNodeMB::Set2(),
           CreateMBlurLeafGrid<N>(bvh,prims.data(),0),bvh->scene->progressInterface,
           prims.data(),pinfo,settings);

        bvh->set(root.ref,root.lbounds,pinfo.size());
      }

      void buildMultiSegment(size_t numPrimitives)
      {
        /* create primref array */
        mvector<PrimRefMB> prims(scene->device,numPrimitives);
        PrimInfoMB pinfo = createPrimRefArrayMSMBlurGrid(scene,prims,bvh->scene->progressInterface);

        /* estimate acceleration structure size */
        const size_t node_bytes = pinfo.num_time_segments*sizeof(AlignedNodeMB)/(4*N);
        //TODO: check leaf_bytes
        //const size_t leaf_bytes = size_t(1.2*Primitive::blocks(pinfo.num_time_segments)*sizeof(SubGridQBVHN<N>));
        const size_t leaf_bytes = size_t(1.2*(float)numPrimitives/N * sizeof(SubGridQBVHN<N>));

        bvh->alloc.init_estimate(node_bytes+leaf_bytes);

        /* settings for BVH build */
        BVHBuilderMSMBlur::Settings settings;
        settings.branchingFactor = N;
        settings.maxDepth = BVH::maxDepth;
        settings.logBlockSize = bsr(sahBlockSize);
        settings.minLeafSize = minLeafSize;
        settings.maxLeafSize = maxLeafSize;
        settings.travCost = travCost;
        settings.intCost = intCost;
        settings.singleLeafTimeSegment = false; //Primitive::singleTimeSegment;
        settings.singleThreadThreshold = bvh->alloc.fixSingleThreadThreshold(N,DEFAULT_SINGLE_THREAD_THRESHOLD,pinfo.size(),node_bytes+leaf_bytes);
        
        /* build hierarchy */
        auto root =
          BVHBuilderMSMBlur::build<NodeRef>(prims,pinfo,scene->device,
                                            RecalculatePrimRef<GridMesh>(scene),
                                            typename BVH::CreateAlloc(bvh),
                                            typename BVH::AlignedNodeMB4D::Create(),
                                            typename BVH::AlignedNodeMB4D::Set(),
                                            CreateMSMBlurLeafGrid<N>(bvh),
                                            bvh->scene->progressInterface,
                                            settings);

        bvh->set(root.ref,root.lbounds,pinfo.num_time_segments);
      }

      void clear() {
      }
    };

    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/
    /************************************************************************************/

#if defined(EMBREE_GEOMETRY_CURVE)
    Builder* BVH4Line4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<4,LineSegments,Line4i>((BVH4*)bvh,scene ,4,1.0f,4,inf); }

#if defined(__AVX__)
    Builder* BVH8Line4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<8,LineSegments,Line4i>((BVH8*)bvh,scene,4,1.0f,4,inf); }
#endif
#endif

#if defined(EMBREE_GEOMETRY_TRIANGLE)
    Builder* BVH4Triangle4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<4,TriangleMesh,Triangle4i>((BVH4*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH4Triangle4vMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<4,TriangleMesh,Triangle4vMB>((BVH4*)bvh,scene,4,1.0f,4,inf); }
#if defined(__AVX__)
    Builder* BVH8Triangle4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<8,TriangleMesh,Triangle4i>((BVH8*)bvh,scene,4,1.0f,4,inf); }
    Builder* BVH8Triangle4vMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<8,TriangleMesh,Triangle4vMB>((BVH8*)bvh,scene,4,1.0f,4,inf); }
#endif
#endif

#if defined(EMBREE_GEOMETRY_QUAD)
    Builder* BVH4Quad4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<4,QuadMesh,Quad4i>((BVH4*)bvh,scene,4,1.0f,4,inf); }
#if defined(__AVX__)
    Builder* BVH8Quad4iMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<8,QuadMesh,Quad4i>((BVH8*)bvh,scene,4,1.0f,4,inf); }
#endif
#endif

#if defined(EMBREE_GEOMETRY_USER)
    Builder* BVH4VirtualMBSceneBuilderSAH    (void* bvh, Scene* scene, size_t mode) {
      int minLeafSize = scene->device->object_accel_mb_min_leaf_size;
      int maxLeafSize = scene->device->object_accel_mb_max_leaf_size;
      return new BVHNBuilderMBlurSAH<4,UserGeometry,Object>((BVH4*)bvh,scene,4,1.0f,minLeafSize,maxLeafSize);
    }
#if defined(__AVX__)
    Builder* BVH8VirtualMBSceneBuilderSAH    (void* bvh, Scene* scene, size_t mode) {
      int minLeafSize = scene->device->object_accel_mb_min_leaf_size;
      int maxLeafSize = scene->device->object_accel_mb_max_leaf_size;
      return new BVHNBuilderMBlurSAH<8,UserGeometry,Object>((BVH8*)bvh,scene,8,1.0f,minLeafSize,maxLeafSize);
    }
#endif
#endif

#if defined(EMBREE_GEOMETRY_INSTANCE)
    Builder* BVH4InstanceMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<4,Instance,InstancePrimitive>((BVH4*)bvh,scene,4,1.0f,1,1); }
#if defined(__AVX__)
    Builder* BVH8InstanceMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAH<8,Instance,InstancePrimitive>((BVH8*)bvh,scene,8,1.0f,1,1); }
#endif
#endif

#if defined(EMBREE_GEOMETRY_GRID)
    Builder* BVH4GridMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAHGrid<4>((BVH4*)bvh,scene,4,1.0f,4,4); }
#if defined(__AVX__)
    Builder* BVH8GridMBSceneBuilderSAH (void* bvh, Scene* scene, size_t mode) { return new BVHNBuilderMBlurSAHGrid<8>((BVH8*)bvh,scene,8,1.0f,8,8); }
#endif
#endif
  }
}