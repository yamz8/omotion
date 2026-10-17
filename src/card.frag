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

// A flat piece, or the picture: as painted, with what the lights add (its
// surface's lie from its mesh, or from how its distances change across the
// screen where the mesh has none),
// fading into the air when farther than the picture had it, and faded out
// as its opacity says.
void MAIN()
{
    vec4 seen = texture(picture, pictureUv);
    float alpha = seen.a * fade;
    vec3 n = length(surface) > 0.001 ? normalize(surface) : normalize(cross(dFdx(worldPos), dFdy(worldPos)));
    if (dot(n, toEye) < 0.0)
        n = -n;
    vec3 colour = seen.rgb + lights(seen.rgb, n, worldPos, toEye);
    FRAGCOLOR = vec4(mix(colour, hazeColour.rgb, haze) * alpha, alpha);
}
