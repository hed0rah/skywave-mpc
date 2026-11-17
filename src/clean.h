// clean.h - cleanup on the receiver's own audio (about 12 kHz, mono), on the worker before the
// jitter buffer: a noise blanker for clicks and impulse noise, an automatic notch for steady
// whistles (heterodynes, open carriers), and an LMS denoiser that keeps what is predictable
// (voice harmonics, tones) and lets the hiss go. None of it runs on the audio thread.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace clean
{

// A sample far above the running mean magnitude closes a gate for 2 ms. The output runs a few
// samples late so the gate is shut before the impulse's leading edge comes out.
class Blanker
{
public:
    void reset()
    {
        std::memset(d_, 0, sizeof(d_));
        w_ = 0;
        avg_ = 0.01f;
        hold_ = shutRun_ = 0;
        shutSum_ = 0.0f;
        gain_ = 1.0f;
    }

    // amount 0 is off; 1 blanks anything 2.5 times the mean magnitude
    float process(float x, float amount)
    {
        d_[w_ & 15] = x;
        const float out = d_[(w_ - kDelay) & 15];
        ++w_;
        if (amount <= 0.0f) { gain_ = 1.0f; hold_ = 0; return out; }
        const float thr = 20.0f * std::exp2(-3.0f * amount);
        const float a = std::fabs(x);
        if (a > thr * avg_ && a > 1e-3f) { hold_ = kHold; ++fired; }
        // an impulse is short: shut this long, the level itself has changed (a signal after silence,
        // a louder station), so the mean takes the level the gate has been seeing and it opens
        if (hold_ > 0) { shutSum_ += a; if (++shutRun_ > kLevelChange) { avg_ = shutSum_ / (float)shutRun_; hold_ = 0; shutRun_ = 0; shutSum_ = 0.0f; } }
        else { shutRun_ = 0; shutSum_ = 0.0f; }
        avg_ += 0.002f * (std::min(a, thr * avg_) - avg_);   // ~40 ms; an impulse moves it no more than the threshold
        avg_ = std::max(avg_, 1e-4f);                         // and silence cannot sink it to where the gate never reopens
        const float target = hold_ > 0 ? 0.0f : 1.0f;
        if (hold_ > 0) --hold_;
        gain_ += (target < gain_ ? 0.5f : 0.1f) * (target - gain_);
        return out * gain_;
    }

    unsigned fired = 0;   // impulses seen, for the panel's lamp

private:
    static constexpr unsigned kDelay = 6;   // 0.5 ms at 12 kHz
    static constexpr int kHold = 24;        // 2 ms
    static constexpr int kLevelChange = 240; // 20 ms
    float d_[16] = {};
    unsigned w_ = 0;
    float avg_ = 0.01f, gain_ = 1.0f;
    int hold_ = 0, shutRun_ = 0;
    float shutSum_ = 0.0f;
};

// Normalised LMS linear predictor: x[n] from x[n-D-N+1 .. n-D]. A steady tone is predictable at any
// delay, noise is not, so the prediction is the tones and the error is everything else.
template <int N>
class Predictor
{
public:
    void reset()
    {
        std::memset(w_, 0, sizeof(w_));
        std::memset(h_, 0, sizeof(h_));
        pos_ = 0;
        pow_ = 0.0f;
    }

    // returns the prediction of x; adapts by mu
    float step(float x, int delay, float mu)
    {
        const unsigned base = pos_ - (unsigned)delay;   // the newest sample the predictor may see
        float y = 0.0f;
        for (int k = 0; k < N; ++k) y += w_[k] * h_[(base - (unsigned)k) & kMask];
        const float e = x - y;
        const float g = mu * e / (pow_ + 1e-6f);
        for (int k = 0; k < N; ++k) w_[k] = 0.99999f * w_[k] + g * h_[(base - (unsigned)k) & kMask];
        h_[pos_ & kMask] = x;
        ++pos_;
        // the next window's power, kept as a running sum (in its newest sample, out this one's oldest),
        // summed afresh now and then so rounding cannot build up
        if ((pos_ & 4095) == 0)
        {
            pow_ = 0.0f;
            for (int k = 0; k < N; ++k) { const float v = h_[(base + 1 - (unsigned)k) & kMask]; pow_ += v * v; }
        }
        else
        {
            const float in = h_[(base + 1) & kMask], out = h_[(base - (unsigned)(N - 1)) & kMask];
            pow_ = std::max(0.0f, pow_ + in * in - out * out);
        }
        return y;
    }

private:
    static constexpr unsigned kLen = 256, kMask = kLen - 1;
    static_assert(N + 64 < (int)kLen, "history too short");
    float w_[N] = {}, h_[kLen] = {};
    unsigned pos_ = 0;
    float pow_ = 0.0f;
};

// the chain: blanker, notch, denoise
class Chain
{
public:
    void reset()
    {
        nb.reset();
        notch_.reset();
        nr_.reset();
        inPow_ = outPow_ = preN_ = postN_ = 0.0f;
    }

    float process(float x, float blank, bool notch, float denoise)
    {
        const float x0 = x;
        x = nb.process(x, blank);
        if (notch)
        {
            const float pre = x;
            x -= notch_.step(x, 24, 0.004f);                  // slow and far back: only what stays put
            preN_ += 0.0005f * (pre * pre - preN_);
            postN_ += 0.0005f * (x * x - postN_);
        }
        else preN_ = postN_ = 0.0f;
        if (denoise > 0.0f)
        {
            const float y = nr_.step(x, 1, 0.02f);
            x += denoise * (y - x);
        }
        else nr_.step(x, 1, 0.0f);                           // keep its history warm
        inPow_ += 0.0005f * (x0 * x0 - inPow_);
        outPow_ += 0.0005f * (x * x - outPow_);
        return x;
    }

    // how much quieter the cleanup made it, in dB, 0 when it is doing nothing
    float removedDb() const { return inPow_ > 1e-12f ? std::max(0.0f, 10.0f * std::log10((inPow_ + 1e-12f) / (outPow_ + 1e-12f))) : 0.0f; }

    // and the notch alone: a whistle it is taking out
    float notchDb() const { return preN_ > 1e-12f ? std::max(0.0f, 10.0f * std::log10((preN_ + 1e-12f) / (postN_ + 1e-12f))) : 0.0f; }

    Blanker nb;

private:
    Predictor<32> notch_;
    Predictor<32> nr_;
    float inPow_ = 0.0f, outPow_ = 0.0f, preN_ = 0.0f, postN_ = 0.0f;
};

} // namespace clean
