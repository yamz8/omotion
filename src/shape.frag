VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;
VARYING vec3 facingNormal;
VARYING vec3 toEye;
VARYING vec3 worldPos;

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

// Where the picture saw the shape it wears the picture; elsewhere its own
// colour, lit at its edges by the light around it. Farther than the picture
// had it, it fades into the air. Solid, with a hard edge, so it hides its
// own far side.
void MAIN()
{
    vec4 seen = texture(picture, pictureUv);
    float alpha = mix(own.a, seen.a, worn);
    if (alpha < 0.5)
        discard;
    vec3 colour = mix(own.rgb, seen.rgb, worn);
    float facing = abs(dot(normalize(facingNormal), normalize(toEye)));
    float rim = pow(1.0 - facing, 3.0) * (1.0 - worn);
    colour = mix(colour, wrap.rgb, clamp(rim * 0.8, 0.0, 1.0));
    vec3 n = normalize(facingNormal);
    if (dot(n, toEye) < 0.0)
        n = -n;
    colour += lights(colour, n, worldPos, toEye);
    colour = mix(colour, hazeColour.rgb, haze);
    FRAGCOLOR = vec4(colour, 1.0);
}
