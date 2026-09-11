// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_runtime.h
 * @brief Binds a container image to a render backend and draws it.
 *
 * WHAT IS COPIED AND WHAT IS NOT
 *
 *   Index buffers and texture pixels are used STRAIGHT OUT OF THE IMAGE - the
 *   container stores them in exactly the layout the renderer wants. On a
 *   flash-resident asset they never enter RAM.
 *
 *   Positions, normals and texcoords ARE decoded into RAM once at bind time,
 *   because they are stored int16-quantized and the renderer wants floats.
 *   Decoding once costs 32 B/vertex of RAM and nothing per frame; decoding per
 *   frame would cost nothing extra in RAM and a multiply-add per component per
 *   frame. Once is the right trade for static meshes, and it is also where a
 *   skinned mesh will write its output every frame, so the buffers are needed
 *   either way.
 *
 *   Node transforms are COPIED out of the image into mutable storage. The image
 *   may sit in flash and animation must be able to overwrite them.
 */
#ifndef A3D_RUNTIME_H_
#define A3D_RUNTIME_H_

#include "a3d_backend.h"
#include "a3d_jobs.h"
#include "a3d_math.h"
#include "a3d_binner.h"
#include "a3d_reader.h"

#include <math.h>     // sqrtf: skinned normals are renormalized per vertex
#include <new>

namespace a3d {

class SceneRuntime
    {
    public:

        SceneRuntime() = default;
        ~SceneRuntime() { release(); }

        SceneRuntime(const SceneRuntime&) = delete;
        SceneRuntime& operator=(const SceneRuntime&) = delete;

        /**
         * Decode meshes, create textures and copy node transforms.
         *
         * The image must outlive the runtime: index buffers and texture pixels
         * are referenced, not copied.
         *
         * @returns false on allocation failure, leaving nothing allocated.
         */
        bool bind(const AssetImage& img, IRenderBackend& be)
            {
            release();
            if (!img.valid()) return false;
            _img = &img;
            _be = &be;

            const uint32_t nNodes = img.nodeCount();
            const uint32_t nMeshes = img.meshCount();
            const uint32_t nMats = img.materialCount();
            const uint32_t nTex = img.textureCount();

            if (nNodes > 0)
                {
                _local = new (std::nothrow) NodeTransform[nNodes];
                _world = new (std::nothrow) float[nNodes * 16];
                if ((_local == nullptr) || (_world == nullptr)) { release(); return false; }
                const fmt::NodeEntry* n = img.nodes();
                for (uint32_t i = 0; i < nNodes; i++)
                    {
                    for (int k = 0; k < 3; k++) { _local[i].t[k] = n[i].translation[k];
                                                  _local[i].s[k] = n[i].scale[k]; }
                    for (int k = 0; k < 4; k++)   _local[i].r[k] = n[i].rotation[k];
                    }
                _nbNodes = nNodes;
                }

            if (nMeshes > 0)
                {
                _meshes = new (std::nothrow) MeshRuntime[nMeshes];
                if (_meshes == nullptr) { release(); return false; }
                _nbMeshes = nMeshes;
                const fmt::MeshEntry* m = img.meshes();
                const fmt::SkinEntry* sk = img.skins();
                const uint32_t nSkins = img.skinCount();

                for (uint32_t i = 0; i < nMeshes; i++)
                    {
                    // A mesh is skinned if some SKIN entry names it. Resolving
                    // that here means _decodeMesh knows whether to decode a bind
                    // pose or leave the buffers for the skinning kernel to fill.
                    const fmt::SkinEntry* mine = nullptr;
                    for (uint32_t s2 = 0; (sk != nullptr) && (s2 < nSkins); s2++)
                        if (sk[s2].mesh_index == i) { mine = &sk[s2]; break; }
                    if (!_decodeMesh(img, be, m[i], mine, _meshes[i]))
                        { release(); return false; }
                    }
                }

            if (img.boneCount() > 0)
                {
                _nbBones = img.boneCount();
                // The bone array keeps its 12-float stride. Widening it to hold
                // the normal matrices as well slowed the RIGID path too, which
                // reads only the first 12: 10.10 -> 13.89 ms on a 34k-vertex
                // model, purely from striding twice as far between bones. The
                // normal matrices go in their own array, and a rig without scale
                // never allocates or reads it.
                _boneMats = new (std::nothrow) float[(size_t)_nbBones * 12];
                if (_boneMats == nullptr) { release(); return false; }
                _normMats = new (std::nothrow) float[(size_t)_nbBones * 9];
                if (_normMats == nullptr) { release(); return false; }
                }

            if (nTex > 0)
                {
                _textures = new (std::nothrow) TextureHandle[nTex];
                if (_textures == nullptr) { release(); return false; }
                _nbTextures = nTex;
                const fmt::TextureEntry* t = img.textures();
                for (uint32_t i = 0; i < nTex; i++)
                    {
                    const void* px = img.at<uint8_t>(t[i].pixels_off, t[i].pixels_size);
                    _textures[i] = (px != nullptr)
                        ? be.createTexture(t[i].width, t[i].height, t[i].format, px)
                        : TextureHandle{};
                    }
                }

            _nbMaterials = nMats;
            updateWorldTransforms();

            // A skinned mesh's vertex buffers are empty until the kernel runs.
            // Skinning once here means the scene is drawable straight after
            // bind() even if the caller never asks for animation.
            SerialExecutor once;
            skinAll(once);
            return true;
            }

        void release()
            {
            if ((_be != nullptr) && (_textures != nullptr))
                for (uint32_t i = 0; i < _nbTextures; i++)
                    if (_textures[i].valid()) _be->destroyTexture(_textures[i]);
            delete[] _textures; _textures = nullptr; _nbTextures = 0;

            if ((_be != nullptr) && (_meshes != nullptr))
                for (uint32_t i = 0; i < _nbMeshes; i++)
                    {
                    _be->freeVec3Array(_meshes[i].positions);
                    _be->freeVec3Array(_meshes[i].normals);
                    _be->freeVec2Array(_meshes[i].texcoords);
                    }
            delete[] _meshes; _meshes = nullptr; _nbMeshes = 0;

            delete[] _boneMats; _boneMats = nullptr;
            delete[] _normMats; _normMats = nullptr; _nbBones = 0;
            delete[] _local; _local = nullptr;
            delete[] _world; _world = nullptr;
            _nbNodes = 0; _nbMaterials = 0;
            _img = nullptr; _be = nullptr;
            }

        bool bound() const { return _img != nullptr; }
        uint32_t nodeCount() const { return _nbNodes; }
        uint32_t meshCount() const { return _nbMeshes; }
        const float* worldMatrix(uint32_t node) const
            { return (node < _nbNodes) ? (_world + node * 16) : nullptr; }

        /** Override a node's local transform. Animation writes through this. */
        void setNodeTRS(uint32_t node, const float t[3], const float r[4], const float s[3])
            {
            if (node >= _nbNodes) return;
            for (int k = 0; k < 3; k++) { _local[node].t[k] = t[k]; _local[node].s[k] = s[k]; }
            for (int k = 0; k < 4; k++)   _local[node].r[k] = r[k];
            }

        void setNodeTranslation(uint32_t node, const float t[3])
            { if (node < _nbNodes) for (int k = 0; k < 3; k++) _local[node].t[k] = t[k]; }

        void setNodeRotation(uint32_t node, const float r[4])
            { if (node < _nbNodes) for (int k = 0; k < 4; k++) _local[node].r[k] = r[k]; }

        void setNodeScale(uint32_t node, const float s[3])
            { if (node < _nbNodes) for (int k = 0; k < 3; k++) _local[node].s[k] = s[k]; }

        /**
         * Recompute world matrices for the whole hierarchy.
         *
         * A single forward pass with no recursion and no stack, which is valid
         * only because the format guarantees parents precede children. That
         * guarantee is asserted by the exporter and checked by the tests.
         */
        /**
         * A transform applied above every root node. Identity by default.
         *
         * An asset carries no opinion about where it belongs in a scene that
         * contains other assets: the Fox is 149 units across and Pikachu is 2.3,
         * and each is authored around its own origin. Without this there is no
         * way to place two of them side by side - a skinned mesh ignores its own
         * node transform by design, so moving the mesh node does nothing.
         *
         *
         * Takes effect on the next updateWorldTransforms(); skinning follows,
         * because bone matrices are built from the world transforms.
         */
        void setRootTransform(const float m[16])
            {
            if (m == nullptr) { mat4Identity(_root); _hasRoot = false; return; }
            mat4Copy(m, _root);
            _hasRoot = true;
            }

        void updateWorldTransforms()
            {
            if ((_img == nullptr) || (_nbNodes == 0)) return;
            const fmt::NodeEntry* n = _img->nodes();
            for (uint32_t i = 0; i < _nbNodes; i++)
                {
                float local[16];
                mat4FromTRS(_local[i].t, _local[i].r, _local[i].s, local);
                const uint16_t p = n[i].parent;
                if ((p == fmt::NONE16) || (p >= i))
                    {
                    if (_hasRoot) mat4Multiply(_root, local, _world + i * 16);
                    else          mat4Copy(local, _world + i * 16);
                    }
                else mat4Multiply(_world + p * 16, local, _world + i * 16);
                }
            }

        /** True when at least one mesh in the scene is skinned. */
        bool hasSkinning() const
            {
            for (uint32_t i = 0; i < _nbMeshes; i++)
                if (_meshes[i].skinned()) return true;
            return false;
            }

        uint32_t boneCount() const { return _nbBones; }

        /**
         * Recompute bone matrices from the current pose and re-skin every
         * skinned mesh. Call after updateWorldTransforms().
         *
         * Splitting across `ex` is safe and produces bit-identical output at any
         * worker count, because each vertex writes only its own slots.
         */
        void skinAll(IJobExecutor& ex)
            {
            if ((_img == nullptr) || (_boneMats == nullptr) || (_nbBones == 0)) return;

            const fmt::BoneEntry* bones = _img->bones();
            if (bones == nullptr) return;

            // Most rigs are rigid: their bone matrices are rotations and
            // translations, and a normal transformed by one comes out unit
            // length already. Reading the separate normal matrix for those costs
            // 1.37x on this stage for no change to a single pixel, so the kernel
            // has two forms and this decides between them once per frame.
            bool scaled = false;

            for (uint32_t b = 0; b < _nbBones; b++)
                {
                const uint16_t node = bones[b].node_index;
                float skinM[16];
                if (node < _nbNodes) mat4Multiply(_world + node * 16, bones[b].inv_bind, skinM);
                else                 mat4Copy(bones[b].inv_bind, skinM);

                // The kernel wants a 3x4 row-major matrix; the scene keeps
                // everything column-major. Transposing once per bone per frame
                // is far cheaper than doing it per vertex.
                float* A = _boneMats + (size_t)b * 12;
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 4; c++)
                        A[r * 4 + c] = skinM[c * 4 + r];

                // A second 3x3 for normals, with each row divided by its own
                // length. A face normal the backend computes itself gets
                // normalized, but a supplied one goes straight to the lighting
                // unnormalized, so a normal
                // that arrives short darkens the surface in proportion - and any
                // scale in the node hierarchy is folded into this matrix. A root
                // scale of 1/171, used to put the Fox and Pikachu at comparable
                // sizes, made the Fox render dark brown and flat instead of
                // orange.
                //
                // Done here, once per bone per frame, rather than per vertex:
                // renormalizing 34,000 skinned normals instead of 156 bones cost
                // 1.5x on the skinning stage and 15% of the frame, because that
                // stage is bandwidth-bound and every added operation lands on
                // every vertex.
                //
                //
                // Exact for the uniform scale a placement transform applies. A
                // non-uniform scale needs the inverse transpose, which glTF
                // discourages on a skinned rig for exactly this reason.
                float* N = _normMats + (size_t)b * 9;
                for (int r = 0; r < 3; r++)
                    {
                    const float x = A[r * 4 + 0], y = A[r * 4 + 1], z = A[r * 4 + 2];
                    const float l2 = x * x + y * y + z * z;
                    const float inv = (l2 > 1e-20f) ? invSqrt(l2) : 1.0f;
                    if ((l2 < 0.998f) || (l2 > 1.002f)) scaled = true;
                    N[r * 3 + 0] = x * inv;
                    N[r * 3 + 1] = y * inv;
                    N[r * 3 + 2] = z * inv;
                    }
                }

            for (uint32_t i = 0; i < _nbMeshes; i++)
                {
                MeshRuntime& mr = _meshes[i];
                if (!mr.skinned() || (mr.positions == nullptr)) continue;
                SkinJob job;
                job.qpos = mr.qpos;
                job.qnrm = mr.qnrm;
                job.bindings = mr.bindings;
                job.bones = _boneMats;
                job.normMats = _normMats;
                job.cx = mr.qcenter[0]; job.cy = mr.qcenter[1]; job.cz = mr.qcenter[2];
                job.pscale = mr.qscale;
                job.outPos = mr.positions;
                job.outNrm = mr.normals;
                ex.parallelFor((int)mr.vertexCount, (scaled ? &_skinRange<true> : &_skinRange<false>), &job);
                }
            }

        // ------------------------------------------------------------------
        // Binned drawing
        //
        // Binning belongs here rather than in the application: the vertex
        // buffers a binner must project are the ones this class owns and
        // rewrites every frame, and the model matrix it must bin against is the
        // one this class is about to draw with. Exposing the buffers instead
        // would make every caller responsible for keeping the two in step.
        // ------------------------------------------------------------------

        /** Nodes that carry a drawable mesh. */
        uint32_t drawableCount() const
            {
            uint32_t n = 0;
            for (uint32_t i = 0; i < _nbNodes; i++) if (_drawableMesh(i) != fmt::NONE16) n++;
            return n;
            }

        /** Vertex and triangle counts of drawable `d`, for sizing a binner. */
        bool drawableSize(uint32_t d, uint32_t& vertices, uint32_t& triangles) const
            {
            const uint32_t node = _drawableNode(d);
            if (node == fmt::NONE32) return false;
            const MeshRuntime& mr = _meshes[_drawableMesh(node)];
            vertices = mr.vertexCount;
            triangles = mr.triangleCount;
            return true;
            }

        /**
         * Bounds of the scene as it will actually be drawn, in world space.
         * Call after updateWorldTransforms() and skinAll(). Returns false if
         * there is nothing to draw.
         *
         * A mesh's stored bbox is in its own local space, which is the wrong
         * answer for anything but a single unskinned mesh at the origin. A
         * skinned character is placed by its skeleton - CesiumMan's root nodes
         * turn it from Z-up to Y-up, so its local bbox is upright in the wrong
         * axis. A scene of parts is placed by node transforms - the city's 167
         * parts all report bounds around their own origins. Both cases put the
         * camera somewhere the model is not.
         */
        /**
         * World bounds of ONE drawable. Exists so a caller can order parts by
         * depth: z is tested before shading, so a nearest-first submission
         * order turns overdraw into a depth compare instead of a texture fetch
         * and a lighting term.
         */
        bool drawableBounds(uint32_t d, float mn[3], float mx[3]) const
            {
            const uint32_t node = _drawableNode(d);
            if ((node == fmt::NONE32) || (_img == nullptr) || (_world == nullptr))
                return false;
            for (int k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }
            return _accumulateNode(node, mn, mx);
            }

        /**
         * World-space bounds over every drawable node.
         *
         * **A SKINNED mesh is bounded by its SKINNED vertices**, which
         * `skinAll()` writes - so for a skinned model this reports the pose of
         * the last `skinAll()`, not of the last `updateWorldTransforms()`. Call
         * it after skinning, or it silently returns the previous pose's box.
         *
         * That is not hypothetical: a check on the Pokemon demo's synthesized
         * idle measured the bounding box across a full animation cycle, moved it
         * by 0.0% on every skinned model, and concluded the animation was doing
         * nothing. The animation was fine; the check had updated the transforms
         * and not the skin. Rendering the frames instead showed 75-92% of the
         * model's pixels changing.
         *
         * An unskinned mesh needs only `updateWorldTransforms()`, and is bounded
         * by its local box's eight transformed corners - an upper bound under
         * rotation, never an under-estimate, which is the safe direction for
         * placing a camera.
         *
         */
        bool worldBounds(float mn[3], float mx[3]) const
            {
            if ((_img == nullptr) || (_world == nullptr)) return false;
            bool any = false;
            for (int k = 0; k < 3; k++) { mn[k] = 1e30f; mx[k] = -1e30f; }

            for (uint32_t node = 0; node < _nbNodes; node++)
                {
                if (_accumulateNode(node, mn, mx)) any = true;
                }
            return any;
            }

        bool _accumulateNode(uint32_t node, float mn[3], float mx[3]) const
            {
                {
                const uint16_t mi = _drawableMesh(node);
                if (mi == fmt::NONE16) return false;
                const MeshRuntime& mr = _meshes[mi];
                if (mr.positions == nullptr) return false;
                const float* W = _world + (size_t)node * 16;

                if (mr.skinned())
                    {
                    // Already world space: skinAll() folds the node world matrix
                    // into every bone matrix.
                    for (uint32_t v = 0; v < mr.vertexCount; v++)
                        _accumulate(mr.positions + (size_t)v * 3, mn, mx);
                    }
                else
                    {
                    // The eight corners of the local bbox, transformed. Larger
                    // than the true bounds under rotation, never smaller, which
                    // is the safe direction for placing a camera.
                    const fmt::MeshEntry& me = _img->meshes()[mi];
                    for (int c = 0; c < 8; c++)
                        {
                        const float p[3] = {
                            (c & 1) ? me.bbox_max[0] : me.bbox_min[0],
                            (c & 2) ? me.bbox_max[1] : me.bbox_min[1],
                            (c & 4) ? me.bbox_max[2] : me.bbox_min[2] };
                        const float q[3] = {
                            W[0] * p[0] + W[4] * p[1] + W[8]  * p[2] + W[12],
                            W[1] * p[0] + W[5] * p[1] + W[9]  * p[2] + W[13],
                            W[2] * p[0] + W[6] * p[1] + W[10] * p[2] + W[14] };
                        _accumulate(q, mn, mx);
                        }
                    }
                }
            return true;
            }

        /**
         * Bin drawable `d` for the pose it will actually be drawn with.
         *
         * The model matrix is pushed to the backend first, so the MVP the binner
         * projects with is the one the rasterizer will use.
         */
        bool binDrawable(uint32_t d, TileBinner& binner)
            {
            if (_be == nullptr) return false;
            float vp[16];
            if (!viewProjection(vp)) return false;
            return binDrawable(d, binner, vp);
            }

        /**
         * The view-projection the backend will rasterize with, with no model
         * matrix in it. Reads it by pushing identity once, so it costs one
         * backend round trip and leaves the backend holding identity.
         */
        bool viewProjection(float out[16]) const
            {
            if (_be == nullptr) return false;
            static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                                                 0, 0, 1, 0, 0, 0, 0, 1 };
            _be->setModelMatrix(kIdentity);
            _be->currentMVP(out);
            return true;
            }

        /**
         * Bin drawable `d` against a caller-supplied view-projection.
         *
         * This touches NO backend state, which is the whole point: every part
         * owns its binner, its arrays and its output, so with the projection
         * passed in rather than read out of a shared renderer, parts can be
         * binned on different cores at the same time. The overload above,
         * which reads the projection from the backend, cannot be - it mutates
         * the renderer's model matrix to do it.
         *
         * The MVP is composed here exactly as the backend would compose it, so
         * the binner still projects with the matrix the rasterizer will use.
         */
        bool binDrawable(uint32_t d, TileBinner& binner, const float viewProj[16]) const
            {
            const uint32_t node = _drawableNode(d);
            if (node == fmt::NONE32) return false;
            const MeshRuntime& mr = _meshes[_drawableMesh(node)];
            if ((mr.positions == nullptr) || (mr.indices == nullptr)) return false;

            float mvp[16];
            mat4Multiply(viewProj, _modelMatrix(mr, node), mvp);
            return binner.bin(mr.positions, (int)mr.vertexCount,
                              mr.indices, (int)mr.triangleCount, mvp);
            }

        /**
         * Draw only the triangles `binner` assigned to `tile`.
         *
         * @param scratch  space for 3 * scratchTriangles uint16 indices.
         * @param backend  render through this backend instead of the bound one.
         * @returns triangles drawn, or -1 if the scratch buffer is too small.
         *
         * SAFE TO CALL CONCURRENTLY for different tiles, provided each caller
         * passes its OWN backend and its OWN scratch buffer. Binning is what
         * makes this true: tiles hold disjoint triangle lists and write disjoint
         * pixels, so nothing is shared but read-only vertex data.
         */
        int drawDrawableTile(uint32_t d, const TileBinner& binner, int tile,
                             uint16_t* scratch, int scratchTriangles,
                             Shading shading, TextureMode textureMode,
                             IRenderBackend* backend = nullptr) const
            {
            IRenderBackend* be = (backend != nullptr) ? backend : _be;
            const uint32_t node = _drawableNode(d);
            if ((node == fmt::NONE32) || (be == nullptr)) return 0;
            const MeshRuntime& mr = _meshes[_drawableMesh(node)];
            if ((mr.positions == nullptr) || (mr.indices == nullptr)) return 0;

            const int n = binner.buildTileIndices(tile, mr.indices, scratch, scratchTriangles);
            if (n <= 0) return n;

            // Hand the backend what binning already produced, if it can take it.
            // The vertices in `binner` were projected with the same matrix this
            // draw would use - binDrawable() composed it from the same
            // view-projection - so they are not an approximation of the second
            // pass, they ARE the second pass.
            if (be->supportsProjectedVertices() &&
                (binner.screenX() != nullptr) && (binner.screenW() != nullptr))
                {
                Material material;
                TextureHandle tex;
                Shading useShading = shading;
                TextureMode useTex = textureMode;
                _prepare(mr, shading, textureMode, material, tex, useShading, useTex, be);
                be->setMaterial(material);
                be->setShading(useShading, useTex);

                ProjectedBatch pb;
                pb.nbTriangles = n;
                pb.indices = scratch;
                pb.screenX = binner.screenX();
                pb.screenY = binner.screenY();
                pb.screenW = binner.screenW();
                pb.behind  = binner.behind();
                if (useShading != Shading::Unlit)
                    { pb.indNormals = scratch; pb.normals = mr.normals; }
                if (useTex != TextureMode::None)
                    {
                    pb.indTexcoords = scratch;
                    pb.texcoords = mr.texcoords;
                    pb.texture = tex;
                    }
                be->drawTrianglesProjected(pb);
                return n;
                }

            Material material;
            TextureHandle tex;
            Shading useShading = shading;
            TextureMode useTex = textureMode;
            _prepare(mr, shading, textureMode, material, tex, useShading, useTex, be);

            be->setModelMatrix(_modelMatrix(mr, node));
            be->setMaterial(material);
            be->setShading(useShading, useTex);

            TriangleBatch batch;
            batch.nbTriangles = n;
            batch.indPositions = scratch;
            batch.positions = mr.positions;
            if (useShading != Shading::Unlit)
                { batch.indNormals = scratch; batch.normals = mr.normals; }
            if (useTex != TextureMode::None)
                {
                batch.indTexcoords = scratch;
                batch.texcoords = mr.texcoords;
                batch.texture = tex;
                }
            be->drawTriangles(batch);
            return n;
            }

        /**
         * Draw every node that carries a mesh.
         *
         * @param shading      requested lighting model; downgraded when the
         *                     backend or the mesh cannot support it.
         * @param textureMode  requested mapping; forced to None for meshes
         *                     without uvs or materials without a texture.
         * @returns number of draw calls issued.
         */
        int draw(Shading shading, TextureMode textureMode,
                 IRenderBackend* backend = nullptr)
            {
            IRenderBackend* be = (backend != nullptr) ? backend : _be;
            if ((_img == nullptr) || (be == nullptr)) return 0;

            if (!be->supports(shading)) shading = Shading::Unlit;
            if (!be->supports(textureMode)) textureMode = TextureMode::None;

            const fmt::NodeEntry* nodes = _img->nodes();
            const fmt::MaterialEntry* mats = _img->materials();
            int calls = 0;

            for (uint32_t i = 0; i < _nbNodes; i++)
                {
                const uint16_t mi = nodes[i].mesh_index;
                if ((mi == fmt::NONE16) || (mi >= _nbMeshes)) continue;
                const MeshRuntime& mr = _meshes[mi];
                if ((mr.positions == nullptr) || (mr.indices == nullptr)) continue;

                Material material;
                TextureHandle tex;
                Shading useShading = shading;
                TextureMode useTex = textureMode;
                _prepare(mr, shading, textureMode, material, tex, useShading, useTex, be);
                (void)mats;

                be->setModelMatrix(_modelMatrix(mr, i));
                be->setMaterial(material);
                be->setShading(useShading, useTex);

                TriangleBatch batch;
                batch.nbTriangles = (int)mr.triangleCount;
                batch.indPositions = mr.indices;
                batch.positions = mr.positions;
                if (useShading != Shading::Unlit)
                    { batch.indNormals = mr.indices; batch.normals = mr.normals; }
                if (useTex != TextureMode::None)
                    {
                    batch.indTexcoords = mr.indices;
                    batch.texcoords = mr.texcoords;
                    batch.texture = tex;
                    }
                be->drawTriangles(batch);
                calls++;
                }
            return calls;
            }

    private:


        static void _accumulate(const float p[3], float mn[3], float mx[3])
            {
            for (int k = 0; k < 3; k++)
                { if (p[k] < mn[k]) mn[k] = p[k]; if (p[k] > mx[k]) mx[k] = p[k]; }
            }

        uint16_t _drawableMesh(uint32_t node) const
            {
            if ((_img == nullptr) || (node >= _nbNodes)) return fmt::NONE16;
            const uint16_t mi = _img->nodes()[node].mesh_index;
            return ((mi == fmt::NONE16) || (mi >= _nbMeshes)) ? fmt::NONE16 : mi;
            }

        uint32_t _drawableNode(uint32_t d) const
            {
            uint32_t seen = 0;
            for (uint32_t i = 0; i < _nbNodes; i++)
                {
                if (_drawableMesh(i) == fmt::NONE16) continue;
                if (seen == d) return i;
                seen++;
                }
            return fmt::NONE32;
            }

        struct NodeTransform { float t[3]; float r[4]; float s[3]; };

        struct MeshRuntime
            {
            float*          positions = nullptr;
            float*          normals = nullptr;
            float*          texcoords = nullptr;
            const uint16_t* indices = nullptr;      // referenced, never copied
            uint32_t        vertexCount = 0;
            uint32_t        triangleCount = 0;
            uint16_t        materialIndex = fmt::NONE16;

            // Skinning source, all referenced straight out of the image.
            //
            // A skinned mesh never decodes its bind pose into RAM: the skinning
            // kernel dequantizes from these int16 streams and blends in one
            // pass, writing straight into `positions` / `normals`. Keeping a
            // separate float bind pose would double the per-vertex RAM for no
            // benefit, since every vertex is rewritten each frame anyway.
            const int16_t*          qpos = nullptr;
            const int16_t*          qnrm = nullptr;
            const fmt::SkinVertex*  bindings = nullptr;
            uint32_t                skeletonIndex = fmt::NONE32;
            float                   qcenter[3] = { 0, 0, 0 };
            float                   qscale = 0.0f;

            bool skinned() const
                { return (bindings != nullptr) && (qpos != nullptr); }
            };

        /** Uniform inputs for one skinned mesh, shared by every worker. */
        struct SkinJob
            {
            const int16_t*         qpos;
            const int16_t*         qnrm;
            const fmt::SkinVertex* bindings;
            const float*           bones;      // 12 floats per bone, row-major 3x4
            const float*           normMats;   // 9 per bone, rows unit; only read when scaled

            float                  cx, cy, cz, pscale;
            float*                 outPos;
            float*                 outNrm;
            };

        /**
         * Two-bone linear blend skinning, dequantizing as it goes.
         *
         * Every vertex writes only its own output slots, which is what makes
         * this safe to split across cores and why the result is bit-identical
         * at any worker count. Phase 0 measured this kernel at 0.53 us/vertex
         * on one P4 core and 0.276 us/vertex on two.
         */
        /**
         * @tparam SCALED  does any bone carry scale?
         *
         * A template rather than a runtime test, because the test has to be
         * outside the loop. Reading `c->scaled` per vertex measured SLOWER than
         * doing the extra work unconditionally - the branch defeats the
         * scheduling of a loop that runs once per vertex.
         */
        template <bool SCALED>
        static void _skinRange(void* ctx, int begin, int end)
            {
            const SkinJob* c = (const SkinJob*)ctx;
            constexpr float SNORM = 1.0f / 32767.0f;
            constexpr float WNORM = 1.0f / 255.0f;

            for (int i = begin; i < end; i++)
                {
                const int i3 = i * 3;

                const float px = c->cx + (float)c->qpos[i3 + 0] * c->pscale;
                const float py = c->cy + (float)c->qpos[i3 + 1] * c->pscale;
                const float pz = c->cz + (float)c->qpos[i3 + 2] * c->pscale;

                const fmt::SkinVertex sv = c->bindings[i];
                const float w0 = (float)sv.weight0 * WNORM;
                const float w1 = 1.0f - w0;

                const float* A = c->bones + (size_t)sv.bone0 * 12;
                const float* B = c->bones + (size_t)sv.bone1 * 12;

                c->outPos[i3 + 0] = w0 * (A[0]*px + A[1]*py + A[2]*pz + A[3])
                                  + w1 * (B[0]*px + B[1]*py + B[2]*pz + B[3]);
                c->outPos[i3 + 1] = w0 * (A[4]*px + A[5]*py + A[6]*pz + A[7])
                                  + w1 * (B[4]*px + B[5]*py + B[6]*pz + B[7]);
                c->outPos[i3 + 2] = w0 * (A[8]*px + A[9]*py + A[10]*pz + A[11])
                                  + w1 * (B[8]*px + B[9]*py + B[10]*pz + B[11]);

                if (c->outNrm == nullptr) continue;
                const float nx = (float)c->qnrm[i3 + 0] * SNORM;
                const float ny = (float)c->qnrm[i3 + 1] * SNORM;
                const float nz = (float)c->qnrm[i3 + 2] * SNORM;
                const float* An = SCALED ? (c->normMats + (size_t)sv.bone0 * 9) : A;
                const float* Bn = SCALED ? (c->normMats + (size_t)sv.bone1 * 9) : B;
                constexpr int st = SCALED ? 3 : 4;   // 3x3 rows, or the 3x4's
                const float ox = w0 * (An[0]*nx + An[1]*ny + An[2]*nz)
                               + w1 * (Bn[0]*nx + Bn[1]*ny + Bn[2]*nz);
                const float oy = w0 * (An[st]*nx + An[st+1]*ny + An[st+2]*nz)
                               + w1 * (Bn[st]*nx + Bn[st+1]*ny + Bn[st+2]*nz);
                const float oz = w0 * (An[2*st]*nx + An[2*st+1]*ny + An[2*st+2]*nz)
                               + w1 * (Bn[2*st]*nx + Bn[2*st+1]*ny + Bn[2*st+2]*nz);

                // NOTE: no per-vertex renormalization here on purpose - the
                // scale is already out, in the per-bone matrix above. What
                // remains is that blending two unit vectors gives a length below
                // one wherever two bones disagree, which is what it has always
                // done and is a few percent at most.
                /* was: renormalize per vertex. A face normal the backend computes itself is normalized
                // but passes a supplied one straight through to the lighting, so
                // a normal that arrives short darkens the surface in proportion.
                //
                // Two effects make them arrive short. Blending two unit vectors
                // gives a length below one whenever the bones disagree - a few
                // percent, small enough to have gone unnoticed. And any scale in
                // the node hierarchy is folded into the bone matrices: with a
                // root scale of 1/171, used to put the Fox and Pikachu at
                // comparable sizes, the normals came out 171 times too short and
                // the Fox rendered dark brown and flat instead of orange.
                */
                c->outNrm[i3 + 0] = ox;
                c->outNrm[i3 + 1] = oy;
                c->outNrm[i3 + 2] = oz;
                }
            }

        /**
         * The matrix to draw mesh `mr` at node `node` with.
         *
         * glTF is explicit that a skinned mesh node's own transform must be
         * IGNORED: skinning already resolves each vertex into world space, so
         * applying the node matrix as well applies it twice. The Fox could not
         * show this because its mesh node is identity. CesiumMan's mesh node
         * sits under a Z-up correction and an armature rotation, and the figure
         * came out lying on its side - drawn without complaint, just wrong.
         *
         * Binning must ask the same question, or it would project with one
         * matrix and rasterize with another.
         */
        const float* _modelMatrix(const MeshRuntime& mr, uint32_t node) const
            {
            static const float kIdentity[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                                                 0, 0, 1, 0, 0, 0, 0, 1 };
            return mr.skinned() ? kIdentity : (_world + (size_t)node * 16);
            }

        /**
         * Resolve the material and settle every downgrade for one mesh.
         *
         * Kept in one place because the binned and unbinned paths must make
         * IDENTICAL choices - the tests compare their framebuffers byte for
         * byte, and two copies of this logic would eventually disagree.
         *
         * Takes the backend explicitly so a tile being drawn on another core
         * queries ITS renderer's capabilities, not the bound one's.
         */
        void _prepare(const MeshRuntime& mr, Shading shading, TextureMode textureMode,
                      Material& material, TextureHandle& tex,
                      Shading& useShading, TextureMode& useTex,
                      IRenderBackend* be) const
            {
            const fmt::MaterialEntry* mats = _img->materials();
            const uint16_t mati = mr.materialIndex;
            if ((mats != nullptr) && (mati != fmt::NONE16) && (mati < _nbMaterials))
                {
                const fmt::MaterialEntry& src = mats[mati];
                for (int k = 0; k < 3; k++) material.color[k] = src.color[k];
                material.ambient = src.ambient;
                material.diffuse = src.diffuse;
                material.specular = src.specular;
                material.specularExponent = (int)src.specular_exponent;
                if ((src.texture_index != fmt::NONE16) && (src.texture_index < _nbTextures))
                    tex = _textures[src.texture_index];
                }

            // Downgrades are decided here rather than left to the backend, so a
            // missing normal or texture produces a correct simpler draw instead
            // of a silently empty one.
            useShading = shading;
            if ((mr.normals == nullptr) && (useShading == Shading::Gouraud))
                useShading = Shading::Flat;
            if ((mr.normals == nullptr) && (useShading == Shading::Flat))
                useShading = Shading::Unlit;
            if (!be->supports(useShading)) useShading = Shading::Unlit;

            useTex = textureMode;
            if ((mr.texcoords == nullptr) || (!tex.valid())) useTex = TextureMode::None;
            }

        static bool _decodeMesh(const AssetImage& img, IRenderBackend& be,
                                const fmt::MeshEntry& m, const fmt::SkinEntry* skin,
                                MeshRuntime& out)
            {
            out.vertexCount = m.vertex_count;
            out.triangleCount = m.triangle_count;
            out.materialIndex = m.material_index;

            out.indices = img.at<uint16_t>(m.indices_off, m.triangle_count * 3);
            if ((out.indices == nullptr) || (m.vertex_count == 0)) return false;

            const int16_t* qp = img.at<int16_t>(m.positions_off, m.vertex_count * 3);
            if (qp == nullptr) return false;
            const int16_t* qn = img.at<int16_t>(m.normals_off, m.vertex_count * 3);
            const AssetImage::MeshQuant q = AssetImage::meshQuant(m);

            if (skin != nullptr)
                {
                const fmt::SkinVertex* sv =
                    img.at<fmt::SkinVertex>(skin->bindings_off, m.vertex_count);
                if (sv != nullptr)
                    {
                    out.bindings = sv;
                    out.qpos = qp;
                    out.qnrm = qn;
                    out.skeletonIndex = skin->skeleton_index;
                    for (int k = 0; k < 3; k++) out.qcenter[k] = q.center[k];
                    out.qscale = q.pscale;
                    }
                }

            out.positions = be.allocVec3Array((int)m.vertex_count);
            if (out.positions == nullptr) return false;
            if (!out.skinned())
                for (uint32_t v = 0; v < m.vertex_count; v++)
                    AssetImage::decodePosition(q, qp + v * 3, out.positions + v * 3);

            if (qn != nullptr)
                {
                out.normals = be.allocVec3Array((int)m.vertex_count);
                if (out.normals == nullptr) return false;
                if (!out.skinned())
                    for (uint32_t v = 0; v < m.vertex_count; v++)
                        AssetImage::decodeNormal(qn + v * 3, out.normals + v * 3);
                }

            const int16_t* qt = img.at<int16_t>(m.texcoords_off, m.vertex_count * 2);
            if (qt != nullptr)
                {
                out.texcoords = be.allocVec2Array((int)m.vertex_count);
                if (out.texcoords == nullptr) return false;
                for (uint32_t v = 0; v < m.vertex_count; v++)
                    AssetImage::decodeTexcoord(qt + v * 2, out.texcoords + v * 2);
                }
            return true;
            }

        const AssetImage*  _img = nullptr;
        IRenderBackend*    _be = nullptr;
        NodeTransform*     _local = nullptr;
        float*             _world = nullptr;
        MeshRuntime*       _meshes = nullptr;
        float              _root[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        bool               _hasRoot = false;
        TextureHandle*     _textures = nullptr;
        float*             _boneMats = nullptr;
        float*             _normMats = nullptr;
        uint32_t           _nbBones = 0;
        uint32_t           _nbNodes = 0;
        uint32_t           _nbMeshes = 0;
        uint32_t           _nbTextures = 0;
        uint32_t           _nbMaterials = 0;
    };

} // namespace a3d

#endif // A3D_RUNTIME_H_
