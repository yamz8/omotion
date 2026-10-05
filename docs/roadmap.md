# Roadmap

## Realism (next)

The Quattro demo (a car flying into the wallpaper) looked like a sticker
moving over a photo: the wheels didn't turn, the dust didn't react, the
motion had no weight. These make a scene react to what moves in it, in order:

1. ~~**Property links.**~~ Done in 0.3.0: `links` in `scene.json`, ⛓ in
   the inspector, `omotion link`. Particle settings (count, speed) aren't
   properties yet, so dust can only thicken through opacity.
2. **Physics motion.** Springs that overshoot and settle, gravity arcs for
   jumps, impacts on landing. Set the start and landing; the path follows.
3. **Layer-attached emitters.** Particles from a point on a layer (a rear
   wheel), only while it touches the ground, inheriting its velocity, then
   slowing, spreading, swirling and fading. Soft lit puffs in scene colours.
4. **Wheel rigs.** Click a wheel: AI select cuts it, fills behind, centres
   the pivot, parents it to the body; it spins from travel and bounces.
5. **Velocity motion blur and contact shadows**, from the actual motion.
6. **Living background.** Slow noise warp of a masked area so painted dust,
   smoke, clouds or water billow (cinemagraph).

Then remake the Quattro: jumps in, lands with a spring, wheels spin, dust
bursts from the tyres, the cloud billows.

## Video layers

New angles (a car turning toward the camera) need invented pixels, which
omotion can't make. Let a layer be a video clip (from a cloud image-to-video
tool, e.g. Veo, Kling, Runway, or Wan on a GPU machine) so it can be cut,
looped and composited with the rest. This laptop (Intel UHD, 7 GB) can't run
the open video models.

## Smaller

- A "highlights only" look, so glow lights up lamps without washing a road.
- Onion-skin ghosts are drawn unbent on pinned layers.
- Adding a pin to a bent layer can shift the rest of the bend slightly.
- Sound playback has only been tested silently; mouse interactions only in
  code.
- The link field in the inspector hasn't been clicked through by hand.

## To discuss with the user

- Omarchy integration: animated wallpaper from an omotion project.
