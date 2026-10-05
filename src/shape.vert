VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;
VARYING vec3 facingNormal;
VARYING vec3 toEye;

void MAIN()
{
    pictureUv = UV0;
    own = COLOR;
    worn = UV1.x;
    facingNormal = normalize(NORMAL_MATRIX * NORMAL);
    toEye = CAMERA_POSITION - (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
