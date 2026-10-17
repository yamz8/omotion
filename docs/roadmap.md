# Roadmap

## The world (now)

The goal: load a picture and it becomes a world you can control and animate,
with its ground, environment and objects, filmed by a camera you can move.
Each stage below has been tried on this laptop (i3-10110U, 7.6 GB, Intel
UHD), with the timings measured there.

1. ~~**World from the picture.**~~ Done (`omotion world`). MoGe-2 ViT-S (MIT, ONNX, 141 MB, 4 s) gives
   every pixel's position in meters, the lens (focal length) and the ground
   plane, and so the camera's height and tilt. The environment becomes 3D
   meshes in layers: the background (with the AI fill behind cut objects,
   depth filled too) and each object on its own, so a moving camera sees
   real parallax without stretching. Rendered with Qt Quick 3D, in the
   window and headless for export (QQuickRenderControl; Intel works).
2. ~~**Objects and camera in meters.**~~ Done (world properties). Each object has a position and turn in
   the world, the camera a position, turn and lens, all keyframed and
   linkable as today. The flat z/perspective of 0.3 becomes a special case.
3. ~~**Objects as 3D shapes.**~~ Done (`omotion shape`); the unseen side
   still wears the picture projected through, and smears when it turns far. TripoSR (MIT, ONNX int8, 430 MB, about 20 s an
   object on this CPU) gives a rough shape; the painting is projected onto
   it from the original camera, so it looks exactly like the picture from
   there and keeps a believable volume when it turns. Optional cloud
   models (Tripo has free credits) for better hidden sides.
4. ~~**Physics.**~~ Done as `omotion drive` (gravity over the world's
   ground, baked to keys; no collisions between pieces yet). The ground from the depth as a collider, gravity in m/s²,
   bodies and vehicles (Qt Quick 3D Physics, PhysX). Taking off, landing
   and touching become events other things react to.
5. **Contact and light.** Shadows and dust are done; still to do: motion
   blur, haze with distance, light wrap, grain. Shadows on the real ground, dust emitted where
   wheels touch (Qt Quick 3D particles), motion blur from movement, haze
   with distance, light wrap and grain.
6. **Directing it.** Next: in the window, orbit and fly the camera and
   move pieces with handles; buttons for world/shape/size/drive. Orbit and fly the camera in the window, move objects
   with handles, and the same in scene.json and the CLI for agents
   ("drive along the road at 70 km/h from 80 m away").

Done since: the unseen side of shapes in the AI's own colours, held to the
outline; depth layers (the background tears at jumps over an AI-painted
layer behind), so the camera can move about a meter; painted dust cut out
and brought back as an animated piece; motion blur.

Also done: haze past the picture's distance (its colour from the farthest
land), light at a shape's unseen edges, film grain.

Next: finding the horizon by itself; clicking pieces in 3D in the window;
collisions between pieces; light wrap on flat pieces' edges.

The 2D engine stays: scenes without a world work as they do.

## A small Blender, AI first (now)

Work the way Blender does: in a 3D view, on objects you pick, with
operators you can find and undo, and curves you can shape. Keep what
Blender doesn't have: a picture is where you start, the AI's steps stay
editable, and you can say what you want in words. Not modelling,
sculpting or node editors.

1. ~~**The 3D view is the editor.**~~ Done. A free view to look at the world from
   outside (orbit, pan, zoom), apart from the shot's camera; clicking a
   piece picks it in 3D; handles move it along an axis and turn it; G
   grabs it, X, Y or Z keeps it to an axis.
2. ~~**Outliner and properties.**~~ Done. The world, its pieces (with what they
   were cut from), the camera, particles and the rest, to pick and hide;
   the selected one's properties beside them, world ones included.
3. ~~**AI steps that stay editable.**~~ Done (old worlds and shapes,
   made before they kept track, count as up to date). Each piece shows its steps (the cut,
   the fill behind it, its shape, the world it's in) with what they were
   made from; any can be done again, and what came after an edited step is
   marked out of date and redone with it.
4. ~~**The graph editor.**~~ Done; easing handles on the curves still to do. The selected layer's animated properties as
   curves over time: drag keys in time and value, see the easing between.
5. ~~**Say what you want.**~~ Done (`F3`). A search box for every operator (Blender's
   F3), and for anything else, words handed to an agent (Claude Code,
   where it's installed) that works through scene.json and the CLI;
   what it changed is shown, one undo takes it all back.

## All of Blender, for the shot (October 2026)

Done, each as a first version: glTF export; rigs (IK, look at, paths,
wheels, empties); curve modifiers, clips and easing handles; pitch and
roll rings, snapping, several selected; rigid bodies; particles from a
layer; the look (grade, vignette, bloom, depth of field); films with
transitions and sound; drawing frame by frame; shape modifiers; lights and
materials; video layers and tracking; imported models; billowing layers.

Done since: bodies tumble (turned boxes meeting the ground and each
other with their corners: they rock, roll over, slide off slopes and rest on
a face; with their own outlines and shapes, so round things roll); models in glTF exports and in physics; films previewed in the
window; clips that add or replace, fading in and out; a dope sheet, a row a
property; several tracked points solving the footage's move (in its
plane: shift, turn, growth) or stabilizing it; cuts and AI select on a
video's frame under the playhead; shadows from added lights (casters as ellipsoids, in the
shaders); the picture as a light probe for models; emitters on a world's pieces throw
particles in 3D; cloth (a layer as fabric, pinned, in the wind); smoke and fire as a 2D
fluid; water that pours, splashes and pools,
meeting layers that are bodies.

Still to do there: fluids worked out in 3D (they're 2D sheets standing in
the world); exact shadow shapes (a caster's shadow is the
rounded shape of its box); a 3D camera solve from tracks (a moving camera's path in depth).

## Realism (folded into the world)

The Quattro demo (a car flying into the wallpaper) looked like a sticker
moving over a photo: the wheels didn't turn, the dust didn't react, the
motion had no weight, and it slid in sideways though the car faces the
camera. A cut-out can only move the way it faces: this shot works as a jump
toward the camera over the crest, a landing, and a bounce. In order:

1. ~~**Property links.**~~ Done in 0.3.0: `links` in `scene.json`, ⛓ in
   the inspector, `omotion link`.
2. **Physics motion.** Springs that overshoot and settle, gravity arcs for
   jumps, bounces on landing; links that follow their source on a spring,
   for suspension, lean and a camera with weight.
3. ~~**A 3D path.**~~ Done: a layer's `z` and the scene's `perspective`
   (vanishing point, focal distance). The camera itself is still flat: it
   pans and zooms, but doesn't move through the depth.
4. **Wheel rigs.** Click a wheel: AI select cuts it, fills behind, centres
   the pivot, parents it to the body. The body sinks on its suspension
   while the wheels stay down. Wheels spin in their own tilted plane (a
   wheel at three-quarters is an ellipse), with a blur around the hub.
5. **Layer-attached emitters.** Particles from a point on a layer (a rear
   wheel), only while it touches the ground, inheriting its velocity, then
   slowing, spreading, swirling and fading. Soft puffs lit from behind.
   Cleaning painted dust and the car out of the background goes with it.
6. **Velocity motion blur and contact shadows**, from the actual motion.
7. **A camera with weight**: a noise source to link to, for shake on impact
   and a hand-held drift.
8. **Belonging to the picture**: light wrap (the background's light
   bleeding round a cut-out's edges), haze toward a colour with distance,
   and film grain over everything.
9. **Living background.** Slow noise warp of a masked area so painted dust,
   smoke, clouds or water billow (cinemagraph).

Then remake the Quattro: it clears the crest, flies at the camera, lands on
a spring, wheels spinning, dust bursting from the tyres, the cloud billowing.

## Video layers (after realism)

New angles (a car turning, sliding sideways, showing its back) need invented
pixels, which only a generative model makes. First let a layer be a video
clip, made by hand in a cloud image-to-video tool (Veo, Kling, Runway, or
Wan on a GPU machine), so it can be cut, looped and composited with the
rest. Calling a provider from inside omotion waits until one is chosen.
This laptop (Intel UHD, 7 GB) can't run the open video models.

## Later

- **Depth warping**: small turns (5–10°) of the scene or one object, by
  bending the picture over its AI depth and filling the edges that open.
- Not planned for now: image-to-3D (slow here, rough on details like a
  livery) and hand-painted extra views (Live2D style).

## Smaller

- A "highlights only" look, so glow lights up lamps without washing a road.
- Particle settings (count, speed) as properties, so links can drive them;
  until then dust reacts only through opacity.
- Onion-skin ghosts are drawn unbent on pinned layers.
- Adding a pin to a bent layer can shift the rest of the bend slightly.
- Dragging a linked pin ignores its link.
- Saved motions keep keys but not links.
- One link per property.
- Links read a parented layer's x and y in its parent's pixels; there's no
  canvas position to link to.
- A layer named "camera" can't be a link's source: the name means the camera.
- Sound playback has only been tested silently; mouse interactions only in
  code; the link field in the inspector hasn't been clicked through by hand.

## To discuss with the user

- Omarchy integration: animated wallpaper from an omotion project.
