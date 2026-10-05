VARYING vec2 pictureUv;

void MAIN()
{
    pictureUv = UV0;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
