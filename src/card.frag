VARYING vec2 pictureUv;
VARYING vec3 worldPos;
VARYING vec3 toEye;
VARYING vec3 surface;

// The lights added to the picture's own: each brightens what faces it,
// falling off with distance, with a highlight as shiny and rough as the
// piece is.
vec3 oneLight(vec3 albedo, vec3 n, vec3 p, vec3 eye, vec3 at, vec3 colour, float reach)
{
    vec3 toLight = at - p;
    float d = length(toLight);
    vec3 l = toLight / max(d, 0.0001);
    float fall = 1.0 / (1.0 + d * d / (reach * reach));
    float diffuse = max(dot(n, l), 0.0);
    vec3 r = reflect(-l, n);
    float highlight = shine * pow(max(dot(r, normalize(eye)), 0.0), mix(80.0, 6.0, roughness)) * step(0.0, diffuse);
    return (albedo * diffuse + vec3(highlight)) * colour * fall;
}

vec3 lights(vec3 albedo, vec3 n, vec3 p, vec3 eye)
{
    return oneLight(albedo, n, p, eye, lightAt0, lightColour0, lightReach0)
         + oneLight(albedo, n, p, eye, lightAt1, lightColour1, lightReach1)
         + oneLight(albedo, n, p, eye, lightAt2, lightColour2, lightReach2)
         + oneLight(albedo, n, p, eye, lightAt3, lightColour3, lightReach3);
}

// Smooth noise, -1 to 1, as the flat drawing's.
float hashOf(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z)) * 2.0 - 1.0;
}

float noise3(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hashOf(i), hashOf(i + vec3(1, 0, 0)), f.x), mix(hashOf(i + vec3(0, 1, 0)), hashOf(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hashOf(i + vec3(0, 0, 1)), hashOf(i + vec3(1, 0, 1)), f.x), mix(hashOf(i + vec3(0, 1, 1)), hashOf(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

// A flat piece, or the picture: as painted, with what the lights add (its
// surface's lie from its mesh, or from how its distances change across the
// screen where the mesh has none),
// fading into the air when farther than the picture had it, and faded out
// as its opacity says.
void MAIN()
{
    // Billowing: where it reads its picture pushed about by drifting noise.
    vec2 uv = pictureUv;
    if (warpAmount > 0.0) {
        vec2 at = vec2(uv.x, 1.0 - uv.y) * warpScale;
        vec2 push = vec2(noise3(vec3(at + warpDrift, warpDrift.x - warpDrift.y)),
                         noise3(vec3(at + vec2(17.3, 4.1) + warpDrift.yx, 9.2 + warpDrift.x)));
        uv += push * warpAmount * vec2(1.0, -warpScale.x / max(warpScale.y, 0.0001));
    }
    vec4 seen = texture(picture, uv);
    float alpha = seen.a * fade;
    vec3 n = length(surface) > 0.001 ? normalize(surface) : normalize(cross(dFdx(worldPos), dFdy(worldPos)));
    if (dot(n, toEye) < 0.0)
        n = -n;
    vec3 colour = seen.rgb + lights(seen.rgb, n, worldPos, toEye);
    FRAGCOLOR = vec4(mix(colour, hazeColour.rgb, haze) * alpha, alpha);
}
