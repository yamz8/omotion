VARYING vec2 pictureUv;

// A flat piece, or the picture: as painted, fading into the air when
// farther than the picture had it, and faded out as its opacity says.
void MAIN()
{
    vec4 seen = texture(picture, pictureUv);
    float alpha = seen.a * fade;
    FRAGCOLOR = vec4(mix(seen.rgb, hazeColour.rgb, haze) * alpha, alpha);
}
