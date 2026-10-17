#pragma once

// Exports a scene as glTF (one .glb file), for Blender or any 3D tool: a
// world's meshes with their pictures on them, each piece and the camera
// moving as they do in the scene; a flat scene as a card for each layer,
// moving as the layer does, seen by a camera that frames the canvas.
// Models imported into a world go in too, as they were made, moved as they
// move. Opacity, looks and particles stay behind: glTF has no place for them.

#include <QString>
#include <QStringList>

#include "scene.h"

namespace Gltf {

// A model that can't be copied is left out, and why is added to skipped.
bool write(const Scene &scene, AssetCache &assets, const QString &path, QString *error, QStringList *skipped = nullptr);

}
