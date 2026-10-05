VARYING vec2 spriteUv;
VARYING vec4 tint;

void MAIN()
{
    spriteUv = UV0;
    tint = COLOR;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
