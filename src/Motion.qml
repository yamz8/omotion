pragma Singleton
import QtQml

// Helpers for scenes, imported with "import Omotion". Every function is a pure
// function of its arguments, so a frame depends on t and nothing else.
QtObject {
    function clamp(x, low, high) {
        return Math.min(high, Math.max(low, x))
    }

    // 0 before start, 1 after end, and linear in between.
    function progress(t, start, end) {
        if (end <= start)
            return t >= start ? 1 : 0
        return clamp((t - start) / (end - start), 0, 1)
    }

    function mix(from, to, amount) {
        return from + (to - from) * amount
    }

    function easeIn(p) {
        return p * p * p
    }

    function easeOut(p) {
        return 1 - Math.pow(1 - p, 3)
    }

    function easeInOut(p) {
        return p < 0.5 ? 4 * p * p * p : 1 - Math.pow(-2 * p + 2, 3) / 2
    }

    // A spring let go at start, travelling from 0 toward 1. It's the closed-form
    // solution of a damped oscillator with unit mass, so any moment can be asked
    // for directly. Stiff and lightly damped snaps and overshoots; soft and
    // heavily damped settles.
    function spring(t, start, stiffness, damping) {
        const k = stiffness === undefined ? 170 : stiffness
        const c = damping === undefined ? 26 : damping
        const x = t - start
        if (x <= 0)
            return 0
        const w0 = Math.sqrt(k)
        const zeta = c / (2 * w0)
        if (zeta < 1) {
            const wd = w0 * Math.sqrt(1 - zeta * zeta)
            return 1 - Math.exp(-zeta * w0 * x) * (Math.cos(wd * x) + zeta * w0 / wd * Math.sin(wd * x))
        }
        if (zeta === 1)
            return 1 - Math.exp(-w0 * x) * (1 + w0 * x)
        const root = Math.sqrt(zeta * zeta - 1)
        const r1 = -w0 * (zeta - root), r2 = -w0 * (zeta + root)
        return 1 - (r2 * Math.exp(r1 * x) - r1 * Math.exp(r2 * x)) / (r2 - r1)
    }

    // A repeatable number in [0, 1) for an integer seed, in place of Math.random().
    function random(seed) {
        let a = (seed | 0) + 0x6D2B79F5
        a = Math.imul(a ^ (a >>> 15), a | 1)
        a ^= a + Math.imul(a ^ (a >>> 7), a | 61)
        return ((a ^ (a >>> 14)) >>> 0) / 4294967296
    }

    // The index of the beat playing at t, or -1 before the first.
    function beatAt(beats, t) {
        let index = -1
        for (let i = 0; i < beats.length; ++i)
            if (beats[i].at <= t)
                index = i
        return index
    }
}
