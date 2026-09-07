# Grand City Parkway — engine integration brief

You are wiring a converted Assetto Corsa track into a game engine. This document is the
whole contract: what each file holds, how to build the world from it, and the traps that
will cost you a day if you meet them by surprise.

Everything here was exported by `ac-car-data` from the paid release of *Grand City Parkway*
by ExPanda. Source: `/home/dominic-asley/dev/assets/ac-maps/grand_city_parkway`.
Exports: `/home/dominic-asley/dev/ac-car-data/out/`.

Read `grand_city_parkway_colliders.json` and `grand_city_parkway_track.json` for exact
counts. Numbers quoted below are for orientation; the manifests are the source of truth.

---

## 0. Units and axes — read this first

* **Metres, kilograms, seconds, newtons.** Nothing is scaled. Do not apply a unit factor.
* **Right-handed, +X left, +Y up, +Z forward.** This is glTF's own convention and AC's,
  winding included. **Do not "fix" the handedness** and do not swap or negate an axis. If
  your engine is left-handed or Z-up, convert at the import boundary and nowhere else.
* **Yaw convention**: `yaw = atan2(direction.x, direction.z)`, so
  `direction = (sin yaw, 0, cos yaw)`. Degrees in the JSON, and +Z is yaw 0.
* World bounds are about **2620 × 33 × 2579 m**. Ground level is **y ≈ 0.003 m**. The road
  dips to about **−24 m** in underpasses and the tallest scenery reaches nearly **300 m**.
  Size your broadphase and your far plane for that.

---

## 1. The files

| file | role | load as |
| --- | --- | --- |
| `grand_city_parkway_track.glb` | world collision: road, sidewalk, markings, ground plane, barriers | static triangle mesh |
| `grand_city_parkway_track.json` | spawn, 46 grid slots, 46 pit slots, friction table, bounds, audit | JSON |
| `grand_city_parkway_visual.glb` | the city, drawn only | render mesh |
| `grand_city_parkway_building_colliders.glb` | building collision — **derived, AC ships none** | static convex hulls |
| `grand_city_parkway_prop_colliders.glb` | street props — **derived**, most of them dynamic | rigid bodies |
| `grand_city_parkway_props_visual.glb` | the drawn half of those props, one node per body | render mesh, parented to the bodies |
| `grand_city_parkway_colliders.json` | manifest for both collider files | JSON |
| `grand_city_parkway_traffic.json` / `_traffic_lanes.csv` | the CSP traffic lane network | JSON / CSV |
| `grand_city_parkway.json` | the source track's own data: surfaces, models, collision inventory | JSON, reference only |

There is a second layout, `freeroam`, exported as `grand_city_parkway_freeroam_*`. It is the
same collision hull and the same city with different pit boxes. Use it only if you want that
spawn set.

---

## 2. World collision — `_track.glb`

29 meshes, 358 086 triangles, 9 materials — one per AC surface. **Physics only. Do not
render it.** It carries plausible colours so you can debug-draw it, and nothing more.

Build one static body from the whole file as a triangle mesh. Keep the per-material split:
the material is how a contact learns what it hit.

Each material's `extras.ac_surface` is the authored surface entry:

```json
{ "key": "ROAD", "friction": 0.98, "damping": 0.0, "is_valid_track": true,
  "is_pitlane": false, "dirt_additive": 0.0, "black_flag_time": 0.0,
  "sin_height": 0.0, "sin_length": 0.0, "vibration_gain": 0.0,
  "vibration_length": 0.0, "wav": null, "ff_effect": "NULL" }
```

Use `friction` for the contact friction. The rest drives tyre audio (`wav`), force feedback
(`ff_effect`, `vibration_*`), dirt pickup (`dirt_additive`) and off-track penalties
(`is_valid_track`, `black_flag_time`).

What is in this file:

| key | triangles | friction | what it is |
| --- | --- | --- | --- |
| WALL | 217 819 | — | street lamps and poles. Solid, not drivable, **no authored friction** |
| KERB | 79 646 | 0.94 | the sidewalks. Most of the city's walkable surface |
| ROAD | 34 407 | 0.98 | the carriageway |
| MARK | 17 013 | 0.93 | the paved strip beside the road; props stand on it |
| GRASS | 6 306 | 0.85 | verges, plus the ground plane |
| RROD | 2 895 | — | thin road-edge strips. Solid, no authored entry |

Three things to know:

1. **`WALL` and `RROD` have no `surfaces.ini` entry.** They are barriers. Give them your
   default wall friction and treat them as non-drivable. `is_barrier` is stated in the
   `barriers` block of `_track.json`.
2. **`1GRASS0.Cylinder.001` is the floor of the world.** 256 triangles spanning the whole
   map and reaching −24 m. Leave the road and a car lands on it. It is why nothing falls out
   of the level.
3. **The mesh is one-sided and its winding was repaired** (190 KERB faces were flipped to
   agree with their own normals). A ray does not care which way a triangle faces but a
   convex sweep does, so do not re-flip anything.

`TILE`, `SAND` and `KSTREE` appear as materials with zero triangles. They are declared by
the track and unused. Ignore them.

---

## 3. Spawning — `_track.json`

```
spawn        source "AC_START_0", position, position_on_surface, direction,
             yaw_degrees, surface_under_spawn, height_above_surface_m
grid.slots   46 entries, same shape, plus index and surface
pits.slots   46 entries
bounds       min / max / size, metres
surfaces[]   the friction table plus triangle counts
audit        per surface: winding agreement, upward fraction
line         null — this map has no AI racing line
```

**Use `position_on_surface`, never `position`.** The authored boxes float: `AC_START_0` sits
1.41 m above the road because AC drops cars onto the grid. Spawn at `position` and every car
starts in the air.

`surface_under_spawn` on this map is `KERB`, which is correct — the grid is laid out on the
paved area, not the carriageway.

There is **no racing line and no pit lane spline**. This is a free-roam map. If you need a
route, use the traffic lanes in section 7.

---

## 4. Scenery — `_visual.glb`

1035 meshes, 17 825 802 triangles, 48 materials, 93 textures, about 1 GB. Render only.

**Materials are Blinn-Phong as the modeller authored them**, not a PBR conversion. Each one
carries:

```json
"extras": {
  "kn5_shader": "ksPerPixelNM",
  "blinn_phong": { "shader": "ksPerPixel", "ambient": 0.0509, "diffuse": 0.8,
                   "ambient_reference": 0.3, "diffuse_reference": 0.2,
                   "specular": 0.8, "specular_exponent": 10.0,
                   "uses_specular_map": false }
}
```

Four rules for reading them:

1. **Divide `ambient` by `ambient_reference` and `diffuse` by `diffuse_reference`.** They are
   coefficients against AC's own lighting constants, and the reference is this asset's
   ordinary material. After dividing, 1.0 means "an ordinary surface taking the ordinary
   amount of light", which you then place from your own measured lighting. Raw, they assert
   that two engines' lighting agrees absolutely, and it does not.
2. **9 materials carry a per-pixel specular map in the metallic-roughness texture slot.**
   `uses_specular_map` says which. **It is not an ORM map**: R multiplies specular, G scales
   the exponent, B is Fresnel. Sample it that way or your glass reads as rusted metal.
3. `baseColorFactor` and `roughnessFactor` are filled in so a plain glTF viewer shows
   something sensible. A Blinn-Phong shader should read `extras.blinn_phong` instead.
4. 4 materials are `alphaMode: MASK` (foliage and railing cutouts) and 4 are double-sided.
   Honour both.

Textures are capped at 1024 px. Six source textures are 8192 px, mostly road line art, so
ask for a re-export at a higher cap if the road looks soft.

---

## 5. Building collision — `_building_colliders.glb`

**AC ships no building collision at all.** In the game you drive through the facades. These
hulls are derived from the drawn geometry by `colliders.py`, and every node says so in
`extras.derived`.

About 2778 buildings, about 8150 convex hulls, all static.

### Node structure — the same for both collider files

```
root  "building_colliders"
 └── "building_0000_building"        translation = centre of mass, extras = the body
      ├── "building_0000_building_hull0"   mesh, vertices LOCAL to the parent
      ├── "building_0000_building_hull1"
      └── …
```

**The transform lives on the body node. The hull children have none.** Read a hull child's
own transform and you will place all eight thousand hulls on the world origin, where they
appear to bury the map. This is the single most likely mistake when importing these files.

**Each hull mesh carries the same name as its node**, so a loader that flattens the node
tree can still group hulls into bodies: everything matching `<body name>_hull<n>` belongs to
`<body name>`. The manifest lists them per body in `hulls`, in child order.

**Hulls are indexed**: the POSITION accessor holds the hull's own points once, and the
indices describe the faces over them. Feed the positions to your convex hull shape and
ignore the triangles. There is **no NORMAL attribute** — glTF requires a renderer to compute
flat normals when one is absent, which is how a collider should look if you debug-draw it.

### Building the shapes

For each body node: create one static body at `translation`, and give it one convex shape
per hull child, built from that child's **vertex positions**. Use the vertex list, not the
triangles — the triangles are the hull's surface and your engine will rebuild its own
face planes anyway. A body with `extras.parts > 1` is a compound: one body, several convex
shapes, each already positioned in the body's local space.

Hull vertex counts are budgeted by object size: 12 for something under 0.8 m, up to 64 for a
tower. Every hull is a closed manifold that fits inside its own bounding box; that is
checked at export.

### What the body extras mean

```json
{ "collider": "convex_hull", "body": "static", "class": "building", "parts": 8,
  "hull_volume_m3": 640.78, "size_m": [33.24, 290.65, 36.39],
  "world_min": [...], "world_max": [...],
  "origin_is": "centre of mass of the hulls; hull vertices are local to it",
  "source_model": "expanda_gcp_build_a.kn5",
  "derived": "convex hull of the drawn geometry — not authored collision data",
  "blocks_road": 0 }
```

`blocks_road` is how many drivable points the hull still contains. Across the whole map it
is **4 of 34 407** carriageway triangle centroids, lifted 0.3 m — one building's worth,
where the geometry genuinely sits over the road. A hull that would have covered a road was
cut along its longest axis — Y included, for a block standing over an underpass — until it
did not, and what survives is reported rather than hidden. Sum `blocks_road` on import; if
it is ever large on another track from this pipeline, do not trust that file.

`hull_volume_m3` on a body is the **sum of its parts**. Sub-hulls of one building can
overlap slightly, so treat it as an indication of bulk, not a measured volume. On a prop it
is the volume the mass was computed from, and props are single-part or nearly so.

**No hull is thinner than 2 cm.** A wall panel modelled with no thickness, or a hull that
came out thinner than that, becomes an axis-aligned box padded to 2 cm on the thin axis: a
car at speed crosses more than a few millimetres in one physics step and would drive
through a sliver. Use continuous collision detection for the car regardless.

Buildings claim no mass. They are level geometry.

---

## 6. Props — `_prop_colliders.glb` and `_props_visual.glb`

3712 bodies: **3536 dynamic** and 176 static, the static ones because they are too big to
shift. Same node structure as the buildings. Total dynamic mass is about 236 t.

### Dynamic body extras

```json
{ "collider": "convex_hull", "body": "dynamic", "class": "lamp_column", "parts": 1,
  "hull_volume_m3": 0.2416, "size_m": [0.213, 7.929, 0.213],
  "world_min": [...], "world_max": [...],
  "mass_kg": 82.2,
  "mass_basis": "340 kg/m3 effective density x 0.242 m3 hull, clamped to the class range",
  "inertia_kgm2": [[425.9, 0.12, -0.0], [0.12, 0.40, -0.34], [-0.0, -0.34, 425.9]],
  "inertia_frame": "about the centre of mass, axes parallel to the world",
  "anchor": { "type": "ground_break", "break_force_n": 18000.0,
              "break_torque_nm": 22000.0,
              "note": "load at which the base gives way and the body comes free" },
  "class_note": "steel lighting column with its head and arm; frangible at the base plate",
  "stands_on": "MARK",
  "origin_is": "centre of mass of the hulls; hull vertices are local to it" }
```

### Build from the manifest, not from node extras

`grand_city_parkway_colliders.json` → `props.items[]` carries the whole physics set for each
body, so a loader that keeps only one extras blob per node never has to dig into the glb:

```json
{ "name": "prop_0000_lamp_column", "class": "lamp_column", "body": "dynamic",
  "mass_kg": 82.2, "parts": 1, "hulls": ["prop_0000_lamp_column_hull0"],
  "size_m": [0.213, 7.929, 0.213], "origin": [-300.101, 3.63, 419.495],
  "inertia_kgm2": [[425.9, 0.12, -0.0], [0.12, 0.40, -0.34], [-0.0, -0.34, 425.9]],
  "anchor": { "type": "ground_break", "break_force_n": 18000.0,
              "break_torque_nm": 22000.0 },
  "stands_on": "MARK", "source_model": "expanda_gcp_road.kn5" }
```

`inertia_kgm2` and `anchor` are **null on a static body** and set on every dynamic one, so
`item["anchor"] is None` is a safe test for "needs no constraint". `origin` matches the glb
node's translation. `hulls` names the meshes to attach, in order. The building rows carry the
same shape minus the mass fields, plus `hull_volume_m3` and `blocks_road`.

### How to build one

1. **Rigid body** at `origin` (the same value as the node `translation`), with `mass_kg`.
2. **Use `inertia_kgm2` as given.** Do not let the engine derive inertia from a uniform-density
   hull. A lamp column's hull is a solid prism and the column is a hollow tube; a derived
   tensor will be several times too large and the post will fall like a felled tree in syrup.
   The tensor is about the centre of mass with axes parallel to the world, and because the
   body's origin *is* the centre of mass, you can hand it straight to an engine that wants a
   local-space tensor at the origin.
3. **Anchor it.** Attach the body to the static world with a fixed constraint that breaks at
   `break_force_n` / `break_torque_nm`. Until it breaks the prop does not move; after it
   breaks the prop is a free rigid body. That is the "ripped out of the ground" behaviour.
4. **Static props** (`body: "static"`) become static bodies. No mass, no anchor.
5. **Sleep them.** Three thousand anchored bodies should be asleep until touched. Anchored
   props never move, so they cost nothing while asleep.

### The classes and what they weigh

| class | count | dynamic | mass | breaks at |
| --- | --- | --- | --- | --- |
| `street_furniture` — mailbox, bin, hydrant, cabinet | 1719 | 1719 | 20–130 kg | 9 kN / 6 kN·m |
| `bench_barrier` — bench, railing run, light barrier | 758 | 663 | 70–450 kg | 14 kN / 12 kN·m |
| `low_furniture` — small planter, kerb box | 529 | 529 | 15–90 kg | 7 kN / 3 kN·m |
| `lamp_column` — steel lighting column with head and arm | 459 | 459 | ~99 kg | 18 kN / 22 kN·m |
| `bollard_post` — bollard, short post, meter stand | 166 | 166 | 18–70 kg | 8 kN / 4 kN·m |
| `heavy_structure` — concrete plinth, stair block, retaining wall | 81 | 0 | static | — |
| `pole_sign` — sign or parking-meter post | 0 here | — | 15–60 kg | 6 kN / 5 kN·m |

**No mass in this table came from the content.** AC states none. Each is an *effective*
density times the hull's volume, clamped per class, and `mass_basis` records the arithmetic
for every body. Effective density, not material density: a lamp column's hull is solid and
the column is a tube, so steel's 7850 kg/m³ would give a 2.4-tonne lamp post. Retune them by
editing `PROP_CLASSES` in `colliders.py` and re-exporting — do not patch the glb.

Anything over 2.5 m³ of hull or 4 m across is forced static whatever its class, and says so
in `extras.static_because`.

### The visual half

`_props_visual.glb` holds the drawn geometry of the same objects. **Every node is named after
its body and its vertices are local to the same origin.** Parent the visual node to the
rigid body and it moves with it. Every body has geometry; there are no orphans.

**You must remove those triangles from `_visual.glb`.** The scenery is batched one mesh per
material, so the painted mailbox is inside it too. Skip that, and knocking a prop over leaves
a copy of it standing. Match by node name and world position from the manifest.

---

## 7. Traffic — `_traffic.json`

This map has no AI racing line. Its road network is CSP's traffic planner data.

```
roles[]        4 lane roles: Parking, Secondary, Main, Highway,
               each with speed_limit_kmh / speed_limit_ms and a right-of-way priority
counts         12 lanes, 35 105.7 m total, 0 intersections, 0 areas
lanes[]        id, name, role, speed_limit_kmh, points, length_m, loop,
               priority_offset, params, elevation, point_list
junctions      derived from geometry, with the distance each match was made on
dead_ends      lanes whose last point joins nothing
```

Four things that will catch you:

1. **Points are sparse control points, not a path.** Two can be 110 m apart and `length_m` is
   the chord length. Interpolate — and note that nothing in the file says how CSP does it, so
   pick a spline and be consistent.
2. **Lane points sit at road level.** Median 1 mm above the collision mesh. Unlike an AI line
   or a grid box, take their `y` as given.
3. **There is no lane graph.** CSP stores none. `junctions` derives one from geometry and this
   map yields exactly **one** branch. Every one of the 12 lanes is listed in `dead_ends`.
4. **Seven of the twelve lanes nearly close on themselves** — 2.0 to 15.3 m from last point to
   first. They are ring roads and the author left the seam open. Close them yourself to get
   loops. The other five are open segments that genuinely end.

All 12 lanes are role `Main`, 56.85 km/h. `_traffic_lanes.csv` is the same data as one row per
control point, with running distance, for anything that would rather read a table.

---

## 8. What is deliberately not here

* **No tree collision.** The author marked the tree model `IS_COLLIDABLE=0` in the CSP config
  and gave `KSTREE` zero friction and `IS_VALID_TRACK=0`. Trees are scenery. Respect that.
* **No AI racing line, no pit lane spline.** None exists in the source.
* **1741 paving-height slabs are dropped**, not exported as static. They are tree pits, grates
  and kerb panels under 0.25 m sitting on the ground. The sidewalk hull already stops a car
  there, and a body would put a kerb where AC has none.
* **No window or interior collision.** Building hulls are the outer shell.

---

## 9. Import checks

Run these once and you will catch every mistake above:

| check | expected |
| --- | --- |
| world bounds of the collision mesh | 2620.3 × 32.6 × 2579.4 m |
| ray down at the spawn | hits `KERB` at y ≈ 0.15 |
| a spawned car at rest | wheels on the surface, not 1.4 m above it |
| building hull positions | spread across the map, **not** clustered at the origin |
| every hull mesh has a name | yes, matching its node, so hulls group into bodies |
| a hull's POSITION count | the hull's own vertex count, fewer than its index count |
| a dynamic manifest row | `inertia_kgm2` and `anchor` both non-null |
| a road-level ray anywhere on the carriageway | hits `ROAD`, never a building hull |
| prop bodies asleep at load | yes; they should wake only on contact |
| a prop's visual | moves with its body when the constraint breaks |
| the scenery | no second copy of a prop left standing |

---

## Appendix — Jolt notes

The document above is engine-agnostic. Four places where Jolt has a specific answer:

* **World collision**: `MeshShapeSettings` from the triangle list, one `Body` with
  `EMotionType::Static`. Per-triangle materials map to `PhysicsMaterial` subclasses, which is
  where the `ac_surface` block belongs; `ContactListener` then reads the material of the hit
  triangle.
* **Convex hulls**: `ConvexHullShapeSettings` takes the vertex array directly. For a body with
  several hulls use `StaticCompoundShapeSettings` with the child hulls at identity, since the
  vertices are already local to the body.
* **Mass and inertia**: build `MassProperties` yourself — set `mMass` and `mInertia` from
  `mass_kg` and `inertia_kgm2` — and set
  `BodyCreationSettings::mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided`.
  Jolt's own `SetMassAndInertiaOfSolidBox` or a hull-derived tensor will be wrong for a hollow
  post.
* **Breakable anchors**: Jolt has no breakable constraint. Create a `FixedConstraint` between
  the prop and the static body, and each step read `GetTotalLambdaPosition()` and
  `GetTotalLambdaRotation()`. Those are impulses: divide by the step to get force and torque,
  compare against `break_force_n` and `break_torque_nm`, and call
  `PhysicsSystem::RemoveConstraint` when either is exceeded. Wake the body when you do.
  Cheaper alternative if you do not want thousands of constraints: leave the props as static
  bodies, and on a contact whose impulse exceeds the threshold, swap the body to
  `EMotionType::Dynamic` and apply the contact impulse.
