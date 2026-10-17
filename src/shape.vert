VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;
VARYING vec3 facingNormal;
VARYING vec3 toEye;
VARYING vec3 worldPos;

void MAIN()
{
    pictureUv = UV0;
    own = COLOR;
    worn = UV1.x;
    facingNormal = normalize(NORMAL_MATRIX * NORMAL);
    worldPos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    toEye = CAMERA_POSITION - worldPos;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
