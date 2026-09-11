// SPDX-FileCopyrightText: 2026 Eric Nam
// SPDX-License-Identifier: Apache-2.0

/**
 * @file a3d_anim.h
 * @brief Clip playback: sample an animation into a scene's node transforms.
 *
 * Animation targets NODES, never bones directly. A bone moves because its node
 * moves, and the skeleton only supplies inverse bind matrices. That is what
 * makes rigid/node animation and skeletal animation the SAME code path here,
 * with skinning as an extra step applied only where a SKIN binding exists.
 *
 * Decoding follows docs/ASSET_FORMAT.md exactly; the host tests compare the
 * results against values the Python exporter wrote.
 */
#ifndef A3D_ANIM_H_
#define A3D_ANIM_H_

#include "a3d_math.h"
#include "a3d_reader.h"
#include "a3d_runtime.h"

namespace a3d {

class AnimationPlayer
    {
    public:

        AnimationPlayer() = default;

        bool bind(const AssetImage& img)
            {
            _img = nullptr; _clip = nullptr; _clipIndex = fmt::NONE32; _timeMs = 0.0f;
            if (!img.valid() || (img.clipCount() == 0)) return false;
            _img = &img;
            return selectClip(0);
            }

        bool bound() const { return _clip != nullptr; }
        uint32_t clipCount() const { return (_img != nullptr) ? _img->clipCount() : 0; }
        uint32_t clipIndex() const { return _clipIndex; }
        float durationMs() const { return (_clip != nullptr) ? (float)_clip->duration_ms : 0.0f; }
        bool looping() const
            { return (_clip != nullptr) && ((_clip->flags & fmt::CLIP_LOOP) != 0); }
        float timeMs() const { return _timeMs; }

        const char* clipName() const
            { return ((_img != nullptr) && (_clip != nullptr))
                     ? _img->string(_clip->name_off) : nullptr; }

        bool selectClip(uint32_t index)
            {
            if ((_img == nullptr) || (index >= _img->clipCount())) return false;
            const fmt::ClipEntry* clips = _img->clips();
            if (clips == nullptr) return false;
            _clip = &clips[index];
            _clipIndex = index;
            _timeMs = 0.0f;
            return true;
            }

        void seek(float timeMs)
            {
            const float d = durationMs();
            if (d <= 0.0f) { _timeMs = 0.0f; return; }
            if (looping())
                {
                // Modulo without fmod, so this header needs no <cmath>.
                float t = timeMs;
                while (t >= d) t -= d;
                while (t < 0.0f) t += d;
                _timeMs = t;
                }
            else
                {
                _timeMs = (timeMs < 0.0f) ? 0.0f : ((timeMs > d) ? d : timeMs);
                }
            }

        void advance(float dtMs) { seek(_timeMs + dtMs); }

        /**
         * Write the current pose into `scene`.
         *
         * Only the channels present in the clip are touched; every other node
         * keeps whatever transform it had, so a clip that animates one arm does
         * not silently reset the rest of the model.
         *
         * @returns the number of channels applied.
         */
        int apply(SceneRuntime& scene) const
            {
            if ((_img == nullptr) || (_clip == nullptr)) return 0;
            const fmt::ChannelEntry* ch =
                _img->at<fmt::ChannelEntry>(_clip->channels_off, _clip->channel_count);
            if (ch == nullptr) return 0;

            const float d = (_clip->duration_ms > 0) ? (float)_clip->duration_ms : 1.0f;
            float frac = _timeMs / d;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            const uint32_t tq = (uint32_t)(frac * 65535.0f + 0.5f);

            int applied = 0;
            for (uint32_t i = 0; i < _clip->channel_count; i++)
                if (_applyChannel(ch[i], tq, scene)) applied++;
            return applied;
            }

    private:

        /** Index of the last key at or before `tq`. Binary search. */
        static uint32_t _keyBefore(const uint16_t* times, uint32_t n, uint32_t tq)
            {
            if (n == 0) return 0;
            if (tq <= times[0]) return 0;
            if (tq >= times[n - 1]) return n - 1;
            uint32_t lo = 0, hi = n - 1;
            while (hi - lo > 1)
                {
                const uint32_t mid = (lo + hi) / 2;
                if (times[mid] <= tq) lo = mid; else hi = mid;
                }
            return lo;
            }

        bool _applyChannel(const fmt::ChannelEntry& ch, uint32_t tq, SceneRuntime& scene) const
            {
            const uint32_t n = ch.key_count;
            if (n == 0) return false;

            const uint16_t* times = _img->at<uint16_t>(ch.times_off, n);
            if (times == nullptr) return false;

            const uint32_t i0 = _keyBefore(times, n, tq);
            const uint32_t i1 = (i0 + 1 < n) ? (i0 + 1) : i0;

            float alpha = 0.0f;
            if ((ch.interp != fmt::ANIM_STEP) && (i1 != i0))
                {
                const uint32_t span = (uint32_t)times[i1] - (uint32_t)times[i0];
                if (span > 0)
                    {
                    const uint32_t clamped = (tq < times[i0]) ? times[i0] : tq;
                    alpha = (float)(clamped - times[i0]) / (float)span;
                    if (alpha > 1.0f) alpha = 1.0f;
                    }
                }

            if (ch.path == fmt::ANIM_ROTATION)
                {
                const int16_t* v = _img->at<int16_t>(ch.values_off, n * 4);
                if (v == nullptr) return false;
                float a[4], b[4], out[4];
                AssetImage::decodeRotationKey(v + i0 * 4, a);
                AssetImage::decodeRotationKey(v + i1 * 4, b);
                quatNlerp(a, b, alpha, out);
                scene.setNodeRotation(ch.target_node, out);
                return true;
                }

            const uint16_t* v = _img->at<uint16_t>(ch.values_off, n * 3);
            if (v == nullptr) return false;
            float a[3], b[3], out[3];
            AssetImage::decodeVec3Key(ch, v + i0 * 3, a);
            AssetImage::decodeVec3Key(ch, v + i1 * 3, b);
            for (int k = 0; k < 3; k++) out[k] = a[k] + (b[k] - a[k]) * alpha;

            if (ch.path == fmt::ANIM_TRANSLATION) scene.setNodeTranslation(ch.target_node, out);
            else                                  scene.setNodeScale(ch.target_node, out);
            return true;
            }

        const AssetImage*     _img = nullptr;
        const fmt::ClipEntry* _clip = nullptr;
        uint32_t              _clipIndex = fmt::NONE32;
        float                 _timeMs = 0.0f;
    };


/**
 * Advance, pose, and re-skin in one call - the whole per-frame animation step.
 * The order is not optional: node transforms must be composed into world space
 * before bone matrices can be built from them.
 */
inline void animateFrame(AnimationPlayer& player, SceneRuntime& scene,
                         IJobExecutor& ex, float dtMs)
    {
    player.advance(dtMs);
    player.apply(scene);
    scene.updateWorldTransforms();
    scene.skinAll(ex);
    }

} // namespace a3d

#endif // A3D_ANIM_H_
