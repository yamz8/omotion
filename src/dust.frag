VARYING vec2 spriteUv;
VARYING vec4 tint;

void MAIN()
{
    vec4 puff = texture(sprite, spriteUv);
    float alpha = puff.a * tint.a;
    FRAGCOLOR = vec4(tint.rgb * alpha, alpha);
}
