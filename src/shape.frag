VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;
VARYING vec3 facingNormal;
VARYING vec3 toEye;

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
    colour = mix(colour, hazeColour.rgb, haze);
    FRAGCOLOR = vec4(colour, 1.0);
}
