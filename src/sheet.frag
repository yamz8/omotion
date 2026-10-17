VARYING vec2 sheetUv;

void MAIN()
{
    // Premultiplied already.
    FRAGCOLOR = texture(picture, sheetUv) * strength;
}
