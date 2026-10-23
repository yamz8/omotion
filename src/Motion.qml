pragma Singleton
import QtQml
import Omotion

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

    // A spring by one of the style guide's motion rules ("micro", "panel",
    // "camera", "headline"), so every object of a class moves alike.
    // Without a guide, the classes move as a new film's guide has them.
    readonly property var defaultRules: ({ micro: [260, 24], panel: [150, 20], camera: [60, 16], headline: [210, 22] })
    function rule(name, t, start) {
        const r = Style.rule(name)
        if (r.stiffness)
            return spring(t, start, r.stiffness, r.damping)
        const d = defaultRules[name]
        return d ? spring(t, start, d[0], d[1]) : spring(t, start)
    }

    // One object across its states: each state is { at, ...values } and the
    // values move to it over "over" seconds before "at" (default 0.4), eased,
    // so the object stays recognizably itself. Numbers interpolate, and so do
    // colors written as "#rrggbb"; anything else switches at the midpoint.
    function between(states, t) {
        if (!states || states.length === 0)
            return ({})
        let i = 0
        while (i + 1 < states.length && states[i + 1].at <= t)
            ++i
        const from = states[i]
        const next = i + 1 < states.length ? states[i + 1] : null
        const result = {}
        for (const key in from)
            result[key] = from[key]
        if (!next)
            return result
        const over = next.over === undefined ? 0.4 : next.over
        const p = easeInOut(progress(t, next.at - over, next.at))
        for (const key in next) {
            if (key === "at" || key === "over")
                continue
            const a = from[key], b = next[key]
            if (typeof a === "number" && typeof b === "number")
                result[key] = mix(a, b, p)
            else if (typeof a === "string" && typeof b === "string" && a[0] === "#" && b[0] === "#")
                result[key] = Qt.rgba(mix(Qt.color(a).r, Qt.color(b).r, p), mix(Qt.color(a).g, Qt.color(b).g, p),
                                      mix(Qt.color(a).b, Qt.color(b).b, p), mix(Qt.color(a).a, Qt.color(b).a, p))
            else
                result[key] = p < 0.5 ? (a === undefined ? b : a) : b
        }
        return result
    }

    // A value for each format, such as tighter type in 1:1:
    // Motion.per(format, { "16:9": 64, "9:16": 72, "1:1": 56 }, 60)
    function per(format, values, fallback) {
        return values && values[format] !== undefined ? values[format] : fallback
    }

    // The part of the frame a platform's own controls leave clear, as
    // { x, y, width, height }: 5% at each edge, and for 9:16 the top 12% and
    // bottom 20%, where Reels and TikTok put their captions and buttons.
    function safe(format, width, height) {
        if (format === "9:16")
            return { x: width * 0.06, y: height * 0.12, width: width * 0.88, height: height * 0.68 }
        return { x: width * 0.05, y: height * 0.05, width: width * 0.9, height: height * 0.9 }
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
