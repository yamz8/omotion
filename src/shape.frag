VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;

// Where the picture saw the shape it wears the picture; elsewhere its own
// colour. Solid, with a hard edge, so it hides its own far side.
void MAIN()
{
    vec4 seen = texture(picture, pictureUv);
    float alpha = mix(own.a, seen.a, worn);
    if (alpha < 0.5)
        discard;
    FRAGCOLOR = vec4(mix(own.rgb, seen.rgb, worn), 1.0);
}
