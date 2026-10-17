VARYING vec2 pictureUv;
VARYING vec3 worldPos;
VARYING vec3 toEye;
VARYING vec3 surface;

void MAIN()
{
    pictureUv = UV0;
    worldPos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    toEye = CAMERA_POSITION - worldPos;
    surface = NORMAL_MATRIX * NORMAL;
    POSITION = MODELVIEWPROJECTION_MATRIX * vec4(VERTEX, 1.0);
}
