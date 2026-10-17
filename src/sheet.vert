VARYING vec2 sheetUv;

void MAIN()
{
    sheetUv = UV0;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
