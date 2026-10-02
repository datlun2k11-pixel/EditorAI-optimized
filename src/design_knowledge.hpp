#pragma once

#include <string_view>

namespace editorai {
// Author-written design guidance, not additional EAS syntax. Keep this shared by
// manual, buffered and tool generation. Research and validation: docs/level-design.md.
inline constexpr std::string_view LEVEL_DESIGN_GUIDANCE = R"design(
DESIGN WORKFLOW (adapt to the request; these are not new EAS commands):
Plan sections by musical phrase or platformer room: gameplay purpose, mode, entry/exit
state, dominant shape, palette roles, focal point, and transition. Establish a short
representative section before repeating it. Reuse its visual language with variations
in silhouette and scale, not identical object stamps across the entire level.

GAMEPLAY MODE CONTRACTS:
- Cube: fixed jump arc; provide takeoff and landing clearance, including headroom.
- Ship: preserve momentum; build smooth corridors with recovery space after portals.
- Ball: gravity swaps need reachable opposing surfaces and time to cross between them.
- UFO: discrete airborne jumps; plan click heights and recovery from early/late clicks.
- Wave: diagonal hold/release path; check both segments and mini size, not cube jumps.
- Robot: vary hold lengths purposefully; allow readable landings and release windows.
- Spider: instantaneous vertical transfer; opposing landing surfaces must overlap X.
- Swing: gravity switching retains momentum; allow room before direction reverses.
Choose modes for their movement, not just variety. Check entry speed, size, gravity,
held input and exit alignment for every transition. Dual requires ONE compatible input
sequence for both players, not two independently passable lanes. Introduce a mechanic,
vary it, combine it, then provide recovery. Difficulty is timing tolerance and control,
not a universal obstacle spacing or a mandatory speed increase.

SYNC AND LEVEL TYPE:
Choose which musical voice inputs, landings and visual accents follow; not every note
needs a hazard. Use get_bpm/get_waveform only when available and distinguish measured
timing from an estimate. Convert time to distance separately across speed changes;
account for song offset, portals and travel direction. Never promise exact sync from
one constant X-per-second calculation.
Platformer uses rooms, navigation, readable goals, return routes and safe retries, not
an auto-scroll obstacle strip. Confirm level type and available mechanics; do not claim
that SECTION or META secretly enables platformer, checkpoints or unsupported triggers.
Wave and Swing are not available in platformer; checkpoints require platformer mode.
Do not use classic X-span as proof of platformer duration or completion.

DECORATION WORKFLOW:
Begin with large shapes and clear gameplay edges, then add material detail and lighting.
Separate palette roles (background, structure, hazard/readable edge, accent), reserving
strong contrast for the player path. Use background silhouettes and selective foreground
framing for depth; preserve intentional empty space. A small repeated motif with meaningful
variation is better than unrelated objects or filling every gap. Decoration density must
fit the requested style; an undecorated layout request must stay a layout.

STYLE RECIPES (choose or combine only those relevant to the request):
- Modern/minimal: broad geometric masses, intentional asymmetry, restrained accents,
  crisp edges and negative space; spend objects on composition rather than tiny fill.
- Glow/neon: dark supporting shapes, narrow bright edges, selective soft light around
  focal elements; keep hazards distinct and avoid stacking glow over the play path.
- Design/tech/industrial: repeat a structural module with trim, joints and inset panels;
  vary module sizes and supports. Machinery belongs to an architectural system.
- Art/nature: establish terrain and a recognizable silhouette first; use coherent
  materials, irregular contours and depth-scaled scenery, not random decorative stamps.
- Pixel/retro: consistent visual pixel size, stepped contours, limited palette and
  reusable tiles; keep decorative pixels separate from the collision skeleton.
- Effect/abstract: develop one shape or motion idea over a phrase; use contrast and
  transitions to reveal structure. Effects must not conceal required inputs.
- Dark/horror: readable foreground silhouettes against subdued scenery; use light to
  direct attention. Darkness must not turn hazards into blind memorization.
- Classic/simple: cohesive block family and strong rhythm; simplicity is intentional,
  not permission for featureless repetition or unrelated saws.

IMPLEMENTATION AND REVIEW:
Use only the documented EAS/JSON fields and catalog IDs. With tools, search_objects for
theme-specific parts instead of restricting every build to the starter catalog. Without
tools, use known names rather than inventing objects. Shaders, particles and complex
logic require actual supported fields; a raw object ID alone does not configure them.
Use notouch on decorative solids AND hazards. Passable means a one-way landing surface,
not collision-free scenery. Alpha/hide changes
visibility, NOT collision. Keep real gameplay cues out of highdetail-only decoration.
Use distinct groups for motion and visibility, and initialize colors before first use.
Supported z_layer values: -5,-3,-1,0,1,3,5,7,9,11; assign editor_layer by purpose.
Review whole-section composition AND close-up hazard readability. If render_level is
available, check that the image actually shows the current draft before judging it.
Check transitions, visual occlusion, collision flags, empty accidental stretches and
unnecessary overlap. Simplify clutter instead of endlessly adding objects. A geometric
passability scan or cube simulation is NOT proof that every mode or dual is playable;
report unsupported checks and reserve verified completion for actual playtesting.
)design";
} // namespace editorai
