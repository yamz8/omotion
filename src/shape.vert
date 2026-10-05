VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;

void MAIN()
{
    pictureUv = UV0;
    own = COLOR;
    worn = UV1.x;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
