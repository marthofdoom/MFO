# Movement first: the build plan (spots, the shared pass, trust, light and darkness)

2026-11-24. Planning only, no code. Written against `origin/main` at `b48bbfcf`, with `origin/feat/sim-v4-pretune` at `a27d9d57` and `origin/feat/n0-probes` at `037dd23c` read for what is in flight.

marth chose "movement first, then the one T1/T2 tune", with light and darkness in it. The design is `iaus-sim-v4.md` sections 8, 10.2, 11, 13, 14 and 15 (15 is on the pretune branch only). This note does not redesign what is ruled. It turns it into packages, says what the design got wrong or left out, and orders the work so the decision code reaches the sim first and the game side follows without moving the tuned behaviour.

Marks used: VERIFIED (read in code or disassembly for this note), DESIGN (from iaus-sim-v4.md, not re-checked), ESTIMATE, PLACEHOLDER, PROBE (must be observed in the field before anything leans on it).

## 0. The short version

- The one rule that makes "sim first, game later, same behaviour" true: **every decision is pure code in `native/iaus`, compiled into the sim, and the tune's cache dies with the sim binary.** So every pure file that can change a decision (spots, the pass, the assignment, trust, the light reader) lands in the sim BEFORE the pre-tune measurements. The game side after that is only fillers and executors: it fills the same structs and carries out the same output. It adds no rule.
- Eleven packages. Five are the sim line and block the tune (S1 to S5). Six are the game line (G0 to G6, G5 is a series). Two build trees are serial inside themselves: the sim tree and the MFO native tree. Harbinger is a third.
- Five things in the design are wrong or missing (section 2). The two that change the plan: ch.24 already has FIVE MFO clients with pairwise precedence rules, so a sixth needs one arbiter first. And the passive N0 point watch cannot answer the parity questions for a general mover, so a dev-build exerciser is needed.
- Light: the cached actor light level is `HighProcessData + 0x3A8` on 1.6.1170 and 1.5.97 (VERIFIED by disassembly, the RE note's "middleHigh" is wrong). A point sampler very likely exists (a lead from the light writer's caller, PROBE). IAUS casts light on himself only because of three separate facts in the candidate code (section 3.3).
- CPU: the spots stage and the pass inside every rollout put the round at an ESTIMATED 1,700 to 2,300 CPU hours against 1,250 today. Section 6 says where that can be cut honestly and what must be measured before the number is trusted.

## 1. What exists today (VERIFIED file:line on origin/main)

| Piece | Where | Note |
|---|---|---|
| Pure geometry, no caller in the DLL | `native/iaus/EnvQuery.h:80` `View`, `:94` `Walker`, `:104` `PathTuning`, `:186` `FindPath`, `:190` `Sight`, `PathSearch.cpp`, `EnvQuery.cpp` | batch W. The sim's world uses it (`tools/iaus_sim/Env.h`, `EnvWorld.h`) |
| The families that move him today | `native/iaus/Core.h` `enum class Family`: `Retreat`, `Sneak`, `Regroup`, `GiveGround`, `Disengage`, `BackOff`, `Step` | generated at `Generate.cpp:181` (Retreat), `:198` (Sneak hold), `:206` (Step), `:237` (GiveGround), `:277` (BackOff) |
| Their roads | `Candidates.cpp:311` (`groundRoad`), `:574` to `:599` (`concealRoad`, `fleeRun`, `backOffRoad`, `stepRoad`), mapped to opcodes at `scheduler/IausSource.cpp:442` to `:449` and `:486` | each road is a Snapshot bool |
| The point picks outside the scorer | `native/Concealment.cpp:60` to `:96` (rings of 12 bearings at 320 and 600 units, step rings 200 and 420, 4 candidates a frame on the main thread, engine rays), `PickStep` `:230`, `NoFoeDetects` `:348`, `PrepareKind` `:508`, `Begin` `:543`, `Service` `:595` | its own constants, not in `Tuning` |
| ch.24 clients, ONE claim per follower | `apmf/CombatApproach.cpp` (held heal, `:166`), `apmf/OffenseApproach.cpp` (`:234` yields to the heal), `apmf/RangeApproach.cpp` (`:10`), `apmf/GroundApproach.cpp` (`:9`), `apmf/Disengage.cpp` (`:20`, `:104` to `:117`) | precedence is written pairwise in five files. "The EARLIER claim keeps the channel" |
| Their entry points | `apmf/APMFBridge.h:722` `ServiceHealApproach`, `:745` `ServiceOffenseApproach`, `:1978` `RequestGroundStep`, `:2018` `RequestDisengage`, `:645` `ServicePursuitLeash`, `:713` `ServiceHideHold` | serviced from `scheduler/CombatScan.cpp:1351` to `:1356` and `scheduler/IausSource.cpp:247`, `:509` |
| The point road in the ABI | `native/APMF_API.h:554` (ch.24), `:835` to `:861` (`kApproach_Hold`, `kApproach_Point`, ABI v39), states `:2466` on | DESIGN 8.3 is right that no new point facet is needed |
| Engine rays | `native/Sightline.h:71` `Check`, `:78` `Want`, `:117` `PointSeenNow(viewer, point, bodyHeight)`, `:129` `TeammateInFireLine` | `PointSeenNow` casts up to 6 rays a call, main thread |
| The navmesh walk that ships | `native/logistics/LootReach.cpp:32` `FindTriangle`, `:59` to `:64` `Collect` (cell `spinLock`, `navMeshes`), `:79` `ComputeOnMain`, `:183` the post | the pattern for the snapshot |
| The pump | `native/Diagnostics.cpp:77` `kPumpMs = 133`, `:1184` `SleeperLoop`, `:1240` the critical `Scheduler::Tick` stage, `:259` and `:327` the perk-effects want and build pass (the main-built pattern, #85). `Scheduler::Tick` is `scheduler/CombatScan.cpp:485` | no decision-window code exists on main (grep). The scheduler task 1dfcuabd439 has not landed |
| Reset | `native/Serialization.cpp:1195` `ResetAllState`, `:1201` `StopPump()` first | every new table is cleared after it |
| Tunables and catalogue | `native/iaus/Core.h:97` `Cons`, `:222` `Tuning`, `:229` `postureMomentum`, `:230` `preemptMargin`, `:396` `Facts`, `:522` `Candidate`. `TuningFile.cpp`, `ClassCurves.h:30` `Scalars`, `Catalogue.cpp:444` to `:455` family weights | the one table rule already holds here |
| Buff candidates | `Generate.cpp:908` to `:916` (party loop, a Self buff at an ally only with `allyRoad`), `Candidates.cpp:262` to `:265` (`allyRoad` asked for the PLAYER only), `cast/BuffRoad.cpp:352` to `:363` (aimed Light refused), `:402` `BuffServedAtAlly`, `Decide.cpp:134` (a buff capped at the best offense score) | section 3.3 |
| Detection | sim `tools/iaus_sim/Detect.h` (the engine formula, one `light` scalar per fight from `Setup.h:132`, `:349`). Native reads the foe group's own record (`iaus/Hide.h`, `Snapshot.h:264` `detectsMe`) | no light fact anywhere in native IAUS |
| N0 probes (branch, lands today) | `native/probes/Probes.h`: `[probe-trav]`, `[probe-drop]`, `[probe-ray]`, `[probe-trust]`, `[probe-point]`. `tools/field/calib_traversal.py` | the point watch observes only the claims MFO already files: give ground and disengage |
| Pretune (branch, in flight) | touches `tools/iaus_sim/Oracle.h`, `Policy.h`, `Load.h`, `LootLab.h`, `sim.cpp`, `tune_driver.py`. The `follow` and `spells` stages live there, not on main | every sim package below rebases on it |

## 2. What the design got wrong or left out

1. **A sixth ch.24 client without an arbiter.** DESIGN 8.3 says "point plus hold is the spot claim". True in the ABI. In MFO the channel is already shared by five files that each know the other four by name. A spot claim added the same way makes thirty pairwise rules. Package G2 puts ONE owner table in front (`apmf/MoveClaim`), first as a pure refactor of today's precedence, then the spot claim is one more row.
2. **N0 cannot measure the general mover.** `[probe-point]` watches give ground (ends on arrival) and the disengage run (hold, with denies). It never sees: a hold at a small radius with foes in reach, a point on another floor or behind a door, a claim kept for many windows, a corner to corner sequence. Those are exactly the parity questions (section 7). They need a claim to be FILED, so they need a dev-build exerciser (G0b). It stays inside the probe rules: no hotkey, config-gated, dev build only, through the existing claim road.
3. **The pass does not fit one pump tick.** DESIGN 8.7 puts 30 to 100 ms of worker time in a 500 ms window. The pump is serial at 133 ms and `Scheduler::Tick` is a critical stage in it. A 100 ms stage delays every follower's action lap. Fix, in S1's contract: the picture is FROZEN at the window's start, phase 1 runs in slices under a per-tick time cap across the window's ticks, phase 3 is one bounded slice at the end. Frozen inputs make the result independent of how it was sliced, so the sim may run the pass in one go and still match.
4. **Rays are dearer than the budget assumed.** The design counts 3 rays a fact. `Sightline::PointSeenNow` casts up to 6 a call. At 16 rays a frame and 12 frames a window that is 32 to 64 (spot, actor) facts a window at the floor, against a 64-spot pool times the foes. The sharing rules of 8.7 still apply, but the pool the pass can keep ANSWERED is nearer 16 to 24 spots than 64. S1 takes the ray budget as a number of FACTS per window (`kMoveBudget.factsPerWindow`, PLACEHOLDER 32 until `[probe-ray]` reports) and the sim runs with the same number. No new engine call is needed for a spot's sight: exposure is "foe sees the point" and the spot's own sight to the target is asked as "target sees the point" (the ray is the same segment).
5. **A spot has no light.** `spotDetected` (8.1) needs the light at the spot. Nothing in the design supplies it. Section 3 does.

Smaller, each folded into a package: the last ranked list the pass pairs with must be exported by `Decide` (S1). The party picture reads follower snapshots that are up to one lap old, so each row carries its age and positions are re-read at the freeze (S1, G3). The 1.5.97 `navMeshes` row and every `BSNavmesh` member row are still unverified (G1). `t.sharedRadiusMargin` and `t.preemptSettle` are not mirrored in the sim (13.3) and both sit next to the new commit rules (S2). The calculate-detection note's risk R1 (an NPC's light refresh outdoors is gated by a byte at +0x328 whose owner struct is not identified) decides whether followers' cached light is live outdoors (G0b).

## 3. Light and darkness

marth: "sneak characters shoudl also be able to utilize nearby darkness levels. And mage characters as well to know when to cast light on others, at the moment sentiewnt AI only seems to light cast on himself. But even once it does others it needs to be able to decast or simply never cast on stealth characters." ClickUp 1dfcuabdwv3.

### 3.1 Engine facts resolved here

| Fact | Result | Grade |
|---|---|---|
| Where the cached light level lives, 1.6.1170 | id 39509 at RVA `0x6EC8B0`: `mov rax,[rcx+0x10]`, `movss xmm0,[rax+0x3A8]`. `AIProcess+0x10` is `high` (fork `RE/A/AIProcess.h:197`). So it is `HighProcessData::lightLevel` at `0x3A8`, as the fork header says (`RE/H/HighProcessData.h:311`). `calculate-detection-re.md:31` says "middleHigh" and is wrong | VERIFIED, disassembly |
| The same on 1.5.97 | the getter's 22 bytes (`48 8B 41 10 48 85 C0 74 09 F3 0F 10 80 A8 03 00 00 C3 0F 57 C0 C3`) occur exactly once in the unpacked 1.5.97 image, file offset `0x6590F0` (RVA `0x659CF0` if the section delta is the 1.6 one, INFERRED). Same two offsets | VERIFIED by byte pattern |
| 1.7.104 | not checked (only the packed exe was looked for). One row to add | open |
| Consequence | only an actor in HIGH process has a light level. Followers and foes in a fight are. A far party member is not: his fact is Unknown, never 0 | follows |
| Can a SPOT be sampled | a lead, not a proof. The light writer's caller (id 39946, RVA `0x713710`) calls `0x1414FCAA0` with a scene object (`[[r15+8]+0x178]+0x70`), a pointer to a stack point (`lea rdx,[rbp-0x21]`), `r8`, and 0, and takes a float back. That is the shape of a luminance-at-a-point query. The fork has no declaration for it | PROBE L1: identify the function on three runtimes, its real signature from the disassembly (principle 6), whether it is safe on the main thread outside the engine's own call, then a `VerifiedAddresses` row and a dev-build line comparing its answer at a follower's own position with his cached level |
| How a Light effect is ended | the fork declares `MagicTarget::DispelEffect(MagicItem*, BSPointerHandle<Actor>& caster, ActiveEffect* = nullptr)` (`RE/M/MagicTarget.h:90`) and `ActiveEffect::Dispel(bool)` (`RE/A/ActiveEffect.h:90`). Nothing in MFO dispels today (grep: flag reads only). Declarations are not ABI proof | PROBE L2: disassemble both on three runtimes, then observe one dispel of a Candlelight in a dev build (does the attached light go at once, does the effect list stay sane) |
| Refresh cadence | every 0.5 s for the player, every 2 s for an NPC (`calculate-detection-re.md:89`). Outdoors an NPC's refresh is gated by a byte at +0x328 (risk R1) | PROBE L3: log each follower's level with interior or exterior and whether it ever changes outdoors |

### 3.2 Mechanism: one reader, two sources, facts like any other

`native/iaus/Light.h`, pure. One reader for "how lit is this place", one for "who must stay dark". Every consumer calls these two and nothing else (the 11-13 rule).

```
namespace MFO::Iaus::Light {
    struct Sample  { Env::Vec3 p; float level; float ageSec; std::uint8_t source; };   // source: actor cached, point sampler, lab
    struct Field   { /* per snapshot grid cell: the freshest samples, bounded */ };
    struct Tunables;                                   // lives in Tuning as t.light*, one table
    float  At(const Field&, Env::Vec3 p, float& a_confidence);          // Unknown when no sample is near: confidence 0
    bool   WantsDark(const Combatant&, const ClassGate&, float& a_why); // sneak class sneaking, hidden, on a hide or step run, or invisible
    float  DetectLightTerm(float level, bool exterior, const DetectGmst&);   // the engine's own light term, shared with the sim's Detect.h
}
```

**Source 1, no new seat: actors are light meters.** Every actor in high process carries a level the engine refreshes itself. The main-thread snapshot pass (the perk-effects pattern, #85) reads `high->lightLevel` for the party, the foes of the fight and the player, and files `(position, level, time)` into the field. Followers walk, so the field fills along their trails and around the foes. That is "pull what can be done": it costs a float read per actor per 2 s.

**Source 2, when L1 proves it: the point sampler.** A budgeted main-thread query for the handful of spots the pass ranks highest for a `WantsDark` follower. It rides the same batch as the rays and is counted against the same per-frame cap. Until L1 passes it does not exist and the reader answers from source 1 alone.

**The facts** (appended to `Facts`, each with one `Cons` row and a curve in the catalogue):

| Fact | Used by | Meaning |
|---|---|---|
| `spotLight`, `spotLightConf` | the pass, kinds hide, hidden step, flank, cover, for a `WantsDark` follower only | the level at the spot. Unknown is `In::Off()`, never dark |
| `spotDetected` (already named in 8.1) | the same | the engine formula with the spot's light and the real ray, per foe that can see the spot |
| `selfLight` | the action facet: the Sneak hold row and Step | a hidden follower standing in bright light scores the hold lower |
| `recipientWantsDark` | `CastBuff` with a Light effect, and scrolls reading one | a VETO input (the catalogue's existing veto shape), not a weight |
| `recipientLight`, `playerLight` | `CastBuff` with a Light effect | the need: how dark it is where the recipient stands |
| `lightOnStealth` | a new small family, `Decast` | one of OUR lights is on someone who now wants dark |

**Decast.** A candidate `Family::Decast` (appended, never renumbered) generated only when a light this party cast is live on a `WantsDark` member. Its action is a dispel of that one effect by (spell, caster). Every engine action belongs to Harbinger, so the dispel is a small Harbinger addition (G6's Harbinger half: one ABI-appended call, verify per runtime, main thread). It never touches a light the player cast himself or one from an item. Until that call exists the family is not generated and "never cast on stealth characters" is the whole behaviour, which marth's sentence allows.

**The same rule outside IAUS.** IAUS is not the only light caster. `cast/Auto.cpp:954` fans a Candlelight to the whole party and `logistics/Service.cpp:1778` casts it out of combat from gambit rows. Both call `Light::WantsDark` for each recipient (G6). One reader, three callers.

### 3.3 Why IAUS lights only himself today (VERIFIED in code, the field split between the causes is not)

1. `Generate.cpp:916`: a Self-delivery buff (Candlelight) is offered at an ally only when `e.allyRoad` is true.
2. `Candidates.cpp:262` to `:265`: `allyRoad` is asked ONCE, with the PLAYER as the recipient, and reused for every ally. `BuffServedAtAlly` (`cast/BuffRoad.cpp:402`) is true only on the claim road: Harbinger present, in combat, the flip proxy available. Out of combat IAUS does not choose at all, the gambit rows do.
3. `cast/BuffRoad.cpp:352` to `:363` and `cast/SpellSupport.cpp:7`: an aimed or location Light (Magelight) is refused by the claim road and hidden from the picker, so it is never an ally candidate.
4. No fact says anyone is in the dark. A light at an ally scores as any buff does (`BoostBuffThreat` on foe count, `Catalogue.cpp:767`) and is capped at the best offense score (`Decide.cpp:134`). The self candidate is first in the party loop and needs no reach or sight test, so it wins and then `activeOn` drops it, and the ally candidates that remain have nothing that raises them.

Which of 1 to 4 bites in a given field case is one log pull (the `[iaus] ... buff ... dropped as a candidate` lines and the candidate dump). It is not needed for the plan: the fix is all four. `allyRoad` per recipient, the need fact, the veto, and the question of Magelight left as it is (an aimed light is Harbinger seat work, not this plan).

### 3.4 In the sim

- A place gets a light field: per triangle, a level. Fixtures draw it (`envs/<name>.txt` gains a light layer). Library places take it from the extract's own light references where the extractor has them, else one level per place from interior or exterior and the hour, as `Setup.h:349` does today. Library data is test input, never game logic.
- `Detect.h` takes the level at the body's triangle instead of the fight's scalar. With the layer off (`--v4 light=off`) every number is today's.
- Light spells: a Light buff sets a radius of light on its recipient in the field, so a lit Thief is detected by the same formula, which is what gives the veto its measured value.
- Scenarios, each with a pass rule: `dark-corner-hide` (a Thief found by a foe picks the hide spot in the dark corner over a nearer lit one, and the lab with `light=off` picks the lit one), `torch-step` (the hidden step never crosses a lit patch while a dark route exists inside budget), `lit-ally-stealth` (a mage with Candlelight and a sneaking Nightblade: zero light casts on the Nightblade, with the veto off the lab reproduces today's detection loss), `light-on-player` (dark room, player not sneaking: the light goes to the player before the mage himself), `decast-on-sneak` (a lit Thief enters sneak: the light is ended within one decision, or, road off, no new one is cast).

## 4. The packages

Sizes are ESTIMATED changed lines with tests and docs. Authors by the ruling of 11-24: Fable planned this, Opus codes tier A and the sim, Sonnet codes tier B from this note. Opus reviews every diff. No package changes a co-save record. None wants one.

### The sim line (blocks the tune)

#### S1. The pure decision core

- **Mechanism.** The party picture, the spot generator, the considerations, the assignment, and the pass as a pure function. No engine type, no caller in the DLL yet (the batch W pattern: in `native/CMakeLists.txt` so MSVC compiles it, exercised by the harness).
- **Files.** New: `native/iaus/PartyPicture.h`, `native/iaus/Spots.h`, `native/iaus/Spots.cpp`, `native/iaus/MovePass.h`, `native/iaus/MovePass.cpp`. Appended: `Core.h:97` (`Cons` rows), `:222` (`Tuning`: `t.spot*`, `moveDecisionSec` with the 0.5 floor in the reader), `:396` (`Facts`), `TuningFile.cpp` (keys, the clamp and its log text), `ClassCurves.h:30` (per-class spot weights), `Catalogue.cpp` (rows and the kind gate), `Decide.cpp` (export the top three of the ranking with their scores), `tools/iaus_harness/harness.cpp` (rows).
- **Data.**

```
struct Spot        { Env::Vec3 p; std::int32_t tri; std::uint32_t kinds; std::uint32_t needs; };   // needs: kNeedPin | kNeedFacing | kNeedGait
struct SpotKindRow { const char* name; SpotGen gen; std::uint64_t families; std::uint32_t needs; };  // kSpotKinds: the ONE table
struct RoadCaps    { bool point, hold, pin, facing, gait; };                                         // what the mover can do NOW
struct PairFact    { Tri sees; float ageSec; };                                                      // keyed (spot, actor), shared by the party
struct Member      { FormID id; Env::Vec3 p; std::int32_t tri; float ageSec; Ranked top[3]; Commit commit; std::uint32_t marks; Abilities abil; };
struct PartyPicture{ std::uint64_t envStamp; double t; std::vector<Member> party; std::vector<Foe> foes; std::vector<Spot> pool; FactTable facts; TrustTable trust; Light::Field light; };
struct MoveBudget  { int spots, paths, factsPerWindow, triples; float sliceMs; };                    // kMoveBudget: the ONE table
struct RayAsk      { std::uint32_t spot; FormID viewer; };
struct Assignment  { FormID who; std::int32_t spot; Family action; std::uint32_t form; float score; };

void  MovePass::Freeze(PartyPicture&, const Env::View&, const Tuning&);                               // window start
bool  MovePass::Phase1(PartyPicture&, const Env::View&, Scratch&, std::vector<RayAsk>&, float a_sliceMs);   // resumable, false = more to do
void  MovePass::Answer(PartyPicture&, std::span<const RayAnswer>);                                    // answers of the window
void  MovePass::Phase3(const PartyPicture&, const ClassGates&, const Tuning&, const RoadCaps&, std::vector<Assignment>&);
```

- **Thread model.** None of its own. It is called by whoever holds the picture: the sim's world, the harness, later the worker (G3). Identical on the three runtimes because it has no runtime in it.
- **Interplay.** Changes no shipped behaviour: no caller. The `Tuning` and `Facts` appends change the sim binary, so this lands before any pre-tune measurement. Pretune touches no file of this package.
- **Failure modes.** A tie that depends on container order (the assignment must sort by score, then FormID, then triangle index, and the harness permutes the party to prove it). A float that differs between gcc and MSVC and flips an assignment (no fast-math, a minimum-gain margin already separates real choices, golden vectors compare assignments exactly and scores to 1e-4). A kind that needs a pin offered while the road cannot pin (the generator reads `RoadCaps`).
- **Acceptance.** Harness rows: order independence under every permutation of four followers, the 0.5 floor clamp, Unknown sight scores as `In::Off()` and never as clear, a committed and unmarked member is not re-scored, one body per portal edge, a budget hit scores fewer spots nearest first and counts it, "stay here" always exists, slicing phase 1 at 1 ms and at 50 ms gives the same assignment, a kind with unmet `needs` is absent. Local MSVC build green.
- **Size and author.** 1,700. Opus. Reviewable alone.

#### S2. The pass in the sim's world

**Built on `feat/move-s2`: `iaus-sim-v4.md` section 17, deviations and open items in 17.6.**

- **Mechanism.** `EnvWorld` runs the pass once per `moveDecisionSec` for the party with the one-window ray latency of 8.7, and executes an assignment through a model of the mover selected by `--position-road point|pinned`. `point` is today's ABI and is the tune's road. The mover's numbers (arrival time against path length, drift inside R, engine-dropped rate, behaviour when a foe is in reach) are rows of `EnvTuning`, PLACEHOLDER until G0b's field data replaces them through `calib_point.py`.
- **Files.** `tools/iaus_sim/EnvWorld.h`, `Env.h` (`V4` switch `spots`, the `EnvView` use), `World.h` (the commit events, the mirrors of `t.sharedRadiusMargin` and `t.preemptSettle`), `Scenarios.h` (the seven of 8.5 get their pass rules), `sim.cpp` (flags, `PathReport` rows), `EnvTest.h`, `calib_point.py` (new), the v4 baseline once.
- **Interplay.** The existing families `Step`, `BackOff`, `GiveGround`, `Retreat`, `Disengage` stay generated and stay the default: each is retired in the lab only when its spot kind reproduces its numbers (the rule of 8.1), under a per-kind switch `--spots-replace <kind list>` so both sides can be run on the same fights. Rebases on pretune.
- **Failure modes.** The lab moving a body by a road the game does not have (only `point` in the tune). A pass that re-decides faster than the floor through an event (an event marks, it never runs a pass). The leash stepping in and out (13.2's `kLeashStandU` must hold for a spot at the leash edge).
- **Acceptance.** The seven scenarios pass their rules with `spots=on` and print today's numbers with `spots=off` (the v4 baseline with `spots=off` compares with 0 changes). `ridge-self-occlusion` reproduces the bad pick with terrain sight off. The six pathing metrics stay inside `kPathBounds` per class. Threads 1 and 16 give the same run. CLI suite once at the end.
- **Size and author.** 1,300. Opus. Needs S1 and the pretune merge.

#### S3. Trust

- **Mechanism.** `native/iaus/EnvTrust.h` pure: `Trust::Of(region)`, the `kTrustSigns` table, decay, the clearing facts, and the two facts `spotRegionTrust` and `pathRegionTrust`. In the sim: the lived signs from the world's own leg ends, the "engine" walking true geometry while IAUS plans on a defective mesh, and a mesh that can change mid fight (13.3 says this is missing and two scenarios need it).
- **Files.** New `native/iaus/EnvTrust.h` (and `.cpp` if it passes 150 lines). `PartyPicture.h` (the table), `Spots.cpp` (the two facts), `tools/iaus_sim/Env.h` (a stamped edit of a loaded place), `EnvWorld.h`, `Scenarios.h`, `harness.cpp`.
- **Failure modes.** The 11-17 GATED lesson again: one false mark freezing a region. Every mark decays, a single lived sign never distrusts, and the scenario proves both.
- **Acceptance.** The four scenarios of 8.9 (`trust-missing-link`, `trust-buried-patch`, `trust-ignored-wall`, `trust-one-way`) inside their gates: recovery time, `osc/min` in band, no permanent block after the repair and the player's walk, the log line once. Plus: one blocked leg in a clean place distrusts nothing. Harness: two different signs, or one repeated, are needed for "distrusted".
- **Size and author.** 800. Opus. Needs S2.

#### S4. Light and darkness, pure and sim half

- **Mechanism.** Section 3.2 and 3.4. `Light.h`, the facts, the veto, the need, `allyRoad` as a per-recipient value in the SNAPSHOT (the pure layer reads `e.allyRoadOn` as a list of recipients, the native filler changes in G6), `Family::Decast` behind `Snapshot::decastRoad` (false until G6), the light layer of places, `Detect.h` by triangle.
- **Files.** New `native/iaus/Light.h`. `Core.h` (`Family` append, `Facts`, `Cons`, `t.light*`), `Snapshot.h:90` (`allyRoad` gains the recipient list, the bool stays for the old filler), `Generate.cpp:908` to `:945` (the veto, the need, per-recipient road), `Catalogue.cpp`, `Spots.cpp` (`spotLight`, `spotDetected`), `tools/iaus_sim/Detect.h`, `Env.h`, `Setup.h:132` and `:349`, `World.h` (a light on a body), `make_fixtures.py`, `Scenarios.h`.
- **Interplay.** `Generate.cpp` is shared with the shipped DLL: with `allyRoadOn` empty and no light field the candidates are byte for byte today's (a harness row pins that on the existing rows). Appending a `Family` moves `kCount`: every `switch` over families and the log's count line (`Candidates.cpp:700`) get the new row, and no stored value holds a family index (to confirm in review: FLWR and MCLS store opcodes, not families).
- **Failure modes.** Unknown light read as dark (it must be `In::Off()`). A veto that also blocks a light on the PLAYER when the player is not sneaking. A mage who relights the moment the Thief stands up and dims it the moment he crouches (the decast and the cast share one hysteresis: `t.lightDarkHoldSec`).
- **Acceptance.** The five light scenarios. `--v4 light=off` compares with 0 changes against the baseline. Harness rows for the veto, the Unknown case and the unchanged-candidates pin.
- **Size and author.** 1,000. Opus. Needs S2. Runs after S3 on the same sim tree.

#### S5. The oracle over spots, and the `spots` stage

- **Mechanism.** 8.4 with the cost cuts of section 6. The oracle's options gain (action, spot). Spots the generator did not pair are offered to the oracle too, and a use of one is a (c) divergence. `ClassRefuses` reads `kSpotKinds` and `RoadCaps`. The `ext:` moves (`Oracle.h:86`: `step_back`, `sprint_back`, `to_player`, `to_healer`, `get_sight`, later `hide`, `flee`) are each retired only after the spot form reproduces its numbers on the same fights. The driver gains `stage_spots` after `follow`: the shared `t.spot*` and `t.light*` scalars, then per-class spot weights, scored on the gap, fenced by J, the class cards and the pathing gates. The regret of the greedy assignment against the oracle is reported.
- **Files.** `tools/iaus_sim/Oracle.h`, `Policy.h`, `sim.cpp`, `tune_driver.py`, `test_tune_driver.py`, `oracle_replay.py`, `live/run_round.py`.
- **Failure modes.** The oracle asked for a position more often than the pass decides (a lab that tunes a creature the game does not have: 8.7). A cut that changes picks without a replay proof. Searching weights for kinds that `needs` a road the game lacks (they are listed as "not searched, with the reason", the pattern of `c21d2341`).
- **Acceptance.** `oracle_replay.py --check` reproduces a dump with spots in it. The cut table of 15.7 redone on spot decisions (same pick share and added loss per rule). `test_tune_driver.py` covers the stage, its fences and the not-searched list. A 12-fight end to end with and without spot racing gives the measured cost factor that replaces section 6's estimate.
- **Size and author.** 1,400. Opus. Needs S2, S3, S4 and pretune.

### The game line

#### G0. N0 as it lands, plus G0b: the exerciser and the light probes

- **Mechanism.** N0 merges as reviewed. G0b adds three things to the dev build only (`MFO_DEV_SELFTEST`, the dev test gambit runs each pending item once, no hotkey, config-gated, rate limited). (1) **The point-road exerciser**: when a fight is live and a row is pending, file ONE `kApproach_Point | kApproach_Hold` claim for one follower through the existing bridge at a point taken from his own recent trail (so it is walkable by proof), at R of 60, 90, 150 and 300, and let `[probe-point]` report it. Rows: a ranged class, a melee class with a foe in reach, a point one floor up along the player's trail, a point past an open door, a claim held 20 s, three points in sequence from a recorded trail. (2) **Light lines**: `[probe-light]` per follower, foe and the player every 2 s while a fight or a party sneak is live: level, interior or exterior, sneaking, detected by whom. This settles risk R1 and gives the light field its first numbers. (3) L1 and L2 as disassembly notes (three runtimes), with the dev-build comparison line for L1 only after its row verifies.
- **Files.** New `native/probes/PointDrive.cpp`, `native/probes/Light.cpp`, `selftest/TestsDrive.cpp` rows, `Config.h` and the INI's `[Probes]` keys, `tools/field/calib_point.py`, `calib_light.py`, the test list. Notes: `Docs/research/2026-11/light-sampler-re.md`.
- **Thread model.** The claim is filed on the worker through the same call `GroundApproach.cpp` makes. The light read is a member read on the MAIN thread (posted, FormID captured and re-resolved), like the perk-effects build. Nothing pending across a save.
- **Interplay.** The exerciser files only when no MFO client holds his ch.24 claim and releases the moment one asks. It is a dev build, so a release DLL has none of it (the hash and strings check of the dev-gambit rule).
- **Failure modes.** A drive that outlives its fight (released on combat end and in `ResetAllState`). A point from a trail in another cell (same cell only).
- **Acceptance.** Local build green, dev and release. Field: one session on Tuxborn gives every row of section 7's table at least five samples.
- **Size and author.** 600. Sonnet for the probes and scripts (tier B: existing road, observes). The L1 and L2 disassembly notes are an Opus research task of their own, small.

#### G1. The active navmesh (N1 as designed)

- **Mechanism.** DESIGN 8.2, 8.6. A main-built immutable `EnvSnapshot` per loaded cell from the `LootReach.cpp:59` walk, published by atomic shared pointer, rebuilt on cell attach, detach and door events. It implements `Env::View`. Static trust signs at build. The dev dump in the sim's `MFOENV` format with `LIVE`. The parity script. Layout rows for `navMeshes` and each `BSNavmesh` member on 1.6.1170, 1.5.97 and 1.7.104, verified in the disassembly, each a row with a self-check. A runtime whose rows fail gets no view and logs it once (the documented degrade).
- **Files.** New `native/env/Snapshot.cpp`, `native/env/Env.h` (public), `native/env/Env_internal.h`, `native/env/Dump.cpp` (dev), `tools/iaus_sim/env_parity.py`. Touched: `Diagnostics.cpp` (a want and build pass beside `:259` and `:327`), `Serialization.cpp:1195` (clear after `StopPump`), `VerifiedAddresses` spec rows, `Docs/VERIFIED-ADDRESSES.md`, MAP, a new invariant.
- **Thread model.** Build on main only. Read anywhere through the shared pointer. The worker never touches a navmesh object.
- **Failure modes.** A build long enough to hitch a frame (bounded triangles per frame, a cell is built over several frames and published whole). A stale snapshot after a door opens (the stamp). Reading a member the header has wrong (principle 6: rows first, then code).
- **Acceptance.** Self-test: the current cell's snapshot is non-empty, the follower's triangle is found, the stamp changes when a door in the cell changes state, the snapshot is gone after a reset. Field: a dump of three cells replays in the sim and the parity script prints its line. `[probe-ray]`'s cost is in hand before the static-sign rays are budgeted.
- **Size and author.** 1,600. Opus, tier A (engine reads, threading). Independent of the sim line except the format.

#### G2. One owner for ch.24

- **Mechanism.** `apmf/MoveClaim`: the one table of who may hold a follower's ch.24 claim. A row is `{ client, rank, may it take from, what it yields to }`. Step 1 is a refactor with no behaviour change: the five existing clients ask the table instead of each other, with today's rules as its rows (the held heal wins, the earlier claim keeps the channel, the disengage drive releases the range close). Step 2 (inside G3) adds the spot row.
- **Files.** New `native/apmf/MoveClaim.cpp`, a block in `apmf/APMFBridge.h`. Touched: the precedence sites in `CombatApproach.cpp:166`, `OffenseApproach.cpp:234`, `RangeApproach.cpp`, `GroundApproach.cpp`, `Disengage.cpp:104` to `:117`.
- **Thread model.** Worker, as the clients are. The table takes the place of locks the files take pairwise today, so the lock order is written down once (review item).
- **Failure modes.** A changed precedence nobody asked for. Proof: every claim, refusal and release log line of a recorded fight is the same before and after (the self-test drive rows for each pair, and one field session's `[approach]` lines compared by script).
- **Acceptance.** Self-test rows for each ordered pair of clients. Local build green. No new log text.
- **Size and author.** 600. Opus (claims and lock order: reviewed at tier A depth). Can run now, beside the sim line.

#### G3. The native pass

- **Mechanism.** The filler and the executor around S1, nothing else. At each window: freeze the picture from the followers' snapshots and their cached rankings, run phase 1 in slices inside the pump under `kMoveBudget.sliceMs`, post the ray asks as one batch to the main thread (a new `Sightline::WantPoints` that spreads `PointSeenNow` calls under the per-frame cap and answers into a table), run phase 3, and hand each assignment to `MoveClaim` as a point and hold claim. The action facet reads `spotAction` and the committed spot back as facts. `[Debug] bIausMovePass` ships 0. `iIausSpotKinds` is a mask of kinds allowed, default the kinds whose `needs` the road meets.
- **Files.** New `native/move/Pass.cpp`, `native/move/Move.h`, `native/move/Move_internal.h`, `selftest/TestsMove.cpp`. Touched: `Diagnostics.cpp` (one stage after `Scheduler::Tick` at `:1240`), `Sightline.h` and `.cpp` (the batch), `scheduler/IausSource.cpp` (cache the ranking, read the two facts), `iaus/Candidates.cpp` (fill the facts), `apmf/MoveClaim.cpp` (the spot row), `Config`, INI reference, `Serialization.cpp:1195`, MAP, INVARIANTS.
- **Thread model, the same on 1.6, 1.5 and 1.7.** Worker: freeze, phases 1 and 3, claims. Main: rays and the light samples, in batches carrying FormIDs and plain points, each batch stamped with a generation that a reset bumps so a late answer is dropped. Combat-thread hooks read nothing of this. Per runtime only the rows of G1 differ. A runtime without a view runs with "stay here" only, which is today's behaviour.
- **Interplay.** With the switch off nothing is filed and no fact changes (a self-test row). With it on, the existing families stay and the pass yields the claim to every existing client by its `MoveClaim` row until G5 moves them over one by one. The combat-style seat (ch.33, `apmf/StyleClaims.cpp`) and the leash (ch.23) are constraints the picture reads, never things the pass files. The scheduler's decision-window task, whichever lands second, aligns `moveDecisionSec` to a whole multiple of the other's window.
- **Failure modes.** A slice that overruns and starves the action laps (the stage is timed by the existing tick-stage macro and logs its own overrun, never silently skips). Answers from a previous window used as fresh (age on every fact). A claim renewed every window that restarts the engine's path each time (keep means no call: section 7, gate P3).
- **Acceptance.** Golden vectors: the sim writes pictures and their assignments (`sim --pass-dump`), the MSVC harness replays them and must give the same assignments. Self-test rows above. Field list below.
- **Size and author.** 2,000. Opus, tier A. Needs S1, G1, G2.

#### G4. Harbinger H1, only what the exerciser shows is missing

Its own repo and brief. Candidates in the order the design lists them: stand still, facing, gait, corridor. One thing to observe FIRST because it may make "stand still" free: a hold at a small R together with a ch.33 combat-style claim that quiets circling and fallback (decision 3). `RoadCaps` gains a bit per item as it lands, and the kinds that `needs` it become available. Opus, tier A, sized after G0b's session.

#### G5. Moving the old rules over, one kind at a time

Each is its own small package (about 250 lines, Sonnet, Opus review, tier B), and each needs the lab proof from S2 and a field session with the new kind on.

| Old rule | Becomes | What stays |
|---|---|---|
| back-off point (`Family::BackOff`, `Concealment` rings) | retreat-with-exit spots for the back-off need | the need (`backOffNeed`), its veto and cooldown, the road gate |
| hidden step (`Family::Step`, `PickStep` `Concealment.cpp:230`) | flank and firing spots for a hidden follower, with `spotLight` and `spotDetected` | `strikeReady`, `HoldStill`, the quiet gait of the disengage drive |
| hide and flee points (`Family::Disengage`, `Retreat`) | cover and retreat-with-exit spots | the flee health line, the run's denies, the hide hold, the blow sink |
| offense approach (`OffenseApproach.cpp`, the 2 s occluded wait) | firing spot, reach the unreachable | the drop cap and backoff as trust inputs, the ref form as the low-trust fallback of 8.9 |
| held-heal approach (`CombatApproach.cpp`) | heal reach | the heal claim, the occluded bound |
| give ground (`GroundApproach.cpp`) | kite path, retreat | the trade test that raises the need |

A kind moves over only when both hold: the lab shows the spot form at least as good on the old rule's own scenario, and the field shows no new end state in that client's log lines. The old code stays behind its switch for one release, then goes.

#### G6. Light, native half

- **Mechanism.** Fill `Light::Field` from the main-built samples (and the point sampler if L1 passed). `allyRoad` per recipient at `Candidates.cpp:262`. `Light::WantsDark` at the two other casters (`cast/Auto.cpp` fan near `:954`, `logistics/Service.cpp` near `:1778`). The Harbinger half: one appended call that ends one effect by (target, spell, caster), verified per runtime, and `Snapshot::decastRoad` true when it is present.
- **Thread model.** Samples read on main, published like the perk table. The dispel is Harbinger's, on main.
- **Failure modes.** Dispelling a light that is not ours. A follower who is `WantsDark` because he crouched for one lap (the hold time). The fan road and IAUS disagreeing (one reader).
- **Acceptance.** Self-test: a sneaking party member is never in the fan's recipient list, the field has samples after ten seconds in a cell. Field list below.
- **Size and author.** 500 native (Sonnet, tier B) plus the Harbinger call (Opus, tier A, small).

#### G7 (later, not before the tune). Traversal, M2 as designed

`iaus/Traversal.h`, the adapter table, the hold-offs of 8.8 item 3, the five scenarios. It waits for `[probe-trav]` and `[probe-drop]` to show a follower doing it (ruling 9a). Its pure reader may land any time after the tune has finished. Opus. 900 native, 600 sim.

## 5. Order, and what runs side by side

Three agents at most. Two trees are serial inside themselves: the sim tree (S2 to S5 touch the same files) and the MFO native tree for packages that share files (G2 before G3, G6 after S4).

| Step | Slot 1 (sim and pure) | Slot 2 (native) | Slot 3 |
|---|---|---|---|
| 1, now | S1 (touches `native/iaus` and the harness only) | G0: N0 merges. Then G0b | pretune bundle A finishes (its own agent, already running) |
| 2 | S2, after S1 and the pretune merge | G2 | G1 |
| 3 | S3, then S4 | field session with G0b, `calib_point.py` and `calib_light.py` (no agent, marth plays) | G1 continues, or L1 and L2 notes |
| 4 | S5 | G3 starts (needs S1, G1, G2) | review slots |
| 5 | pre-tune measurements ONCE, then THE TUNE (holds no build tree) | G3, then G6 | G4 in the Harbinger tree |
| 6 | | G5 series, one kind per field session | G7 when the probes show a traversal |

The game line never waits for the tune and the tune never waits for the game line. What ties them is section 7.

## 6. What the sim needs before the tune can start, and the CPU

**Before the pre-tune measurements, merged in the sim binary:** S1, S2, S3, S4, S5, and pretune bundle A. Then the one-time measurements of 15.7 run on that binary, with one more: the pathing bounds (`--path-oracle`) are measured with `spots=on`. After that the binary is frozen for the round. G0b's field numbers are wanted before the freeze because they are `EnvTuning` rows, but they do not block it: section 7 says what happens to a value tuned on a placeholder.

**Not needed before the tune:** G1 to G6, traversal, collision extraction, Harbinger additions.

**Cost.** Today's estimate for the round is about 1,250 CPU hours, between 950 and 1,800 (15.7). Movement adds two things.

| Added cost | ESTIMATE | Why |
|---|---|---|
| The pass inside every rollout, in every stage | 1.2 to 1.5 times a rollout | a rollout is 12 to 15 percent sight and path today. The pass adds spot generation, paths and scoring twice a second |
| Spot options in the oracle's race | 1.3 to 1.6 times the rollouts of a decision in the stages where spots are raced | 8.4 says 2 to 3 times. The cuts below bring it down |
| The `spots` stage itself | 250 to 400 CPU hours for the 15 Beta 1 cells, about 2.5 times that for all 38 | built like `follow`: search on IAUS fights, the oracle only to accept and verify |
| The round | **1,700 to 2,300 CPU hours** (16 cores at idle priority: about 4.5 to 6 days, against 3.3) | to be replaced by S5's measured factor before the round starts |

Where it can be cut without making the oracle think less:

1. **Static spots once per place.** Cover, high ground, chokepoint and exits are properties of the mesh and the door stamp. Compute them at load, share them across every fight and rollout in that place. Exact.
2. **The game's own fact reuse, mirrored.** A (spot, actor) sight fact is reused while neither end left its triangle. The game does this for its ray budget, so the lab doing it is the model, not a shortcut.
3. **Race spots only where the pass would decide.** At a window where the follower is committed and unmarked the oracle has no position option either. With a commit per leg that is most windows.
4. **Race spots only in the stages that are about them:** `spots` and the final `gap`. In `follow` and `spells` the oracle keeps its action options and every rollout still moves by the pass.
5. **The existing pair rule and twins** take spots in one triangle cluster as what they are. Proven by the replay, as the action cuts were.
6. **Search cheap, verify dear.** The shared scalars are searched on IAUS fights against J and the pathing gates (no oracle), then each accepted value gets the oracle sets, as `follow` does.
7. **Places that have something to decide.** The stage draws its fights from environment classes where today's IAUS has nonzero `holdUnreach`, `reblock` or occluded time, plus the scenario fixtures. An open room teaches a spot weight nothing. The held-out check still uses every class.
8. The knobs of 15.7 in its order: `--cells 15` first is about 40 percent of the round.

Not offered as cuts: a smaller pool for the oracle than for IAUS (it would hide the (c) divergences), a lower ceiling (marth's call, and 15.7's table shows what it costs).

## 7. Parity: can the game do what the oracle tuned

The risk: the tune finds weights for a body that reaches a point in the time `FindPath` says and stands on it. The engine may take another route, wander inside R, or drop the path. Three defences, in order of strength.

**(a) The tune runs on the road the game has.** `--position-road point` only. Kinds whose `needs` include pin, facing or gait are refused to IAUS and to the oracle alike under it, so no weight is tuned for a thing the game cannot do. `pinned` is a measurement that prices each H1 item, nothing else.

**(b) The mover's numbers are measured, and the tune says which it stood on.** The model's rows and their field source:

| Gate | Question | Measured by | Pass |
|---|---|---|---|
| P1 arrival | time to arrive against `FindPath` length over his speed | G0b rows, `[probe-point]`, later the dump's travelled length | median ratio inside 0.8 to 1.3 (PLACEHOLDER) |
| P2 hold | share of time inside R at R of 60, 90, 150, 300, and what he does in there | G0b | the smallest R with 90 percent inside becomes the spot radius the lab uses |
| P3 keep | does a standing claim that is not re-filed keep him there for 20 s, and does re-filing restart his path | G0b | keep = no call. If the claim needs a heartbeat that restarts pathing, that is an H1 item before any switch |
| P4 dropped | engine-dropped share on points taken from a walked trail, and later on `FindPath`-reachable points | G0b, then G3 with the switch on in a dev build | under 5 percent (PLACEHOLDER), and each drop is a lived trust sign |
| P5 melee | a melee class holding a point with a foe in reach keeps attacking | G0b | yes. If he stops, melee kinds are point without hold |
| P6 rays | geometry sight against the engine's verdict at the same positions | G1's dump with `LIVE` | the 5 percent of ruling 8, per environment class |
| P7 ray cost | what an engine ray and a `PointSeenNow` cost | `[probe-ray]` (N0) | sets `kMoveBudget.factsPerWindow` |
| P8 light | does a follower's cached level move outdoors, does it match the sampler | `[probe-light]`, L1 | decides source 1 alone or both |

**(c) What stays behind a switch until its gate passes.**

| Tuned value or behaviour | Switch | Opens when |
|---|---|---|
| everything the pass does | `bIausMovePass` (ships 0) | P1 to P5 and P7 measured, G3 reviewed, and a re-check below |
| each spot kind | `iIausSpotKinds` mask | its `needs` met by `RoadCaps`, and for a kind that replaces an old rule, its G5 package |
| cover, chokepoint hold, flank, high ground with a pin | the same mask, off | P2 shows a small R pins him, or H1 lands a pin |
| the `t.light*` weights for spots | `bIausSpotLight` | P8 |
| the light veto and the need on casts | none: it ships on with S4's values | it needs no mover and no sampler. Unknown light is not dark |
| decast | `Snapshot::decastRoad` | the Harbinger call and L2 |
| drops and parkour edges | `--v4 drops`, ruling 9a | `[probe-drop]` and `[probe-trav]` show one |

**The re-check.** If the measured P1, P2 or P4 lands outside the placeholder the tune used, the tuned spot weights are NOT shipped as they are. `calib_point.py` rewrites the `EnvTuning` rows, and only `stage_spots` is rerun on the new binary (ESTIMATE 250 to 400 CPU hours for Beta 1). The rest of the round stands only if the `follow` gap on a sample of fights moves less than its noise on the new binary. That check is cheap and is the rule for whether a full round is owed. So the cheapest path is the obvious one: play the G0b session before the freeze.

## 8. Acceptance, in one place

- **Sim scenarios.** The seven of 8.5 with their pass rules (S2). The four trust scenarios of 8.9 and the single-blocked-leg negative (S3). The five light scenarios of 3.4 (S4). The five traversal scenarios of 8.8 (G7). Each also runs with its mechanism off and must reproduce the old behaviour, which is the proof the scenario tests the mechanism.
- **Harness rows** (`tools/iaus_harness`): the list under S1, plus trust thresholds (S3), the light veto and the unchanged-candidates pin (S4), and the golden vectors (G3).
- **Self-test rows** (dev DLL): the snapshot rows (G1), each ordered pair of ch.24 clients (G2), the pass files nothing with the switch off, a reset mid window leaves no claim and no pending answer, the 0.5 floor clamp logs once (G3), the fan's recipient list (G6).
- **Field list** (each an item on the in-game test list when its package merges): the G0b rows. An archer whose target steps behind a wall moves within one commit time and fires. A melee follower blocked in a doorway is not joined by a second one. A healer round a corner walks to reach. A retreat never ends in a dead end while another way exists. No follower stands at the foot of a tower looking up. A Thief found by a foe hides in the darker of two corners. A mage lights the player in a dark room and never a sneaking party member. A lit Thief who crouches loses the light (after G6). On a weird cell one `[env-trust]` line names the sign and he still fights. Frame time with a party of ten and the pass on shows no step against the pass off.

## 9. Decisions for marth

1. **Which spot kinds may ship before a pin exists.** Recommendation: tune every kind the point road can execute, and keep cover, chokepoint hold, flank and pinned high ground off in the mask until the exerciser shows a small radius pins him or Harbinger gains a pin. Their weights are then a short second stage, not part of this tune.
2. **Does the player count as a stealth character.** Recommendation: yes. While the player sneaks no follower lights him, and a light a follower cast on him is ended. A light the player cast himself is never touched.
3. **May a held spot quiet his circling through the combat-style claim.** It would make "stand still" cost no new Harbinger facet. It removes no attack, but it does scale down movement the engine would have done, and your rule is that new locomotion never removes moves. Recommendation: allow it in the dev exerciser only, look at the footage and the numbers, then rule.
4. **Where sneak characters use darkness.** Recommendation for this plan: in combat and on the hidden approach (hide, step, flank spots prefer the dark). Following the player through shadow out of combat is logistics movement and belongs to the later logistics run.
5. **Who a mage lights.** Light does nothing for a follower's own fighting. It helps the player see. Recommendation: the player first when the player's own level is low and he is not sneaking, then members fighting within the player's view, never anyone who wants dark, and not as a combat priority over a heal or a ward (the existing buff cap stays).

## 10. Riskiest points

1. The mover. Everything in section 7. A tune run before the G0b session stands on placeholders and may owe a `spots` rerun.
2. Rays. The pool the game can keep answered is small (section 2 item 4). If `[probe-ray]` says a fact costs more than assumed, the pass sees fewer spots than the oracle's lab did, unless the lab runs with the same budget. It must.
3. The assignment across compilers. One flipped tie between gcc and MSVC and the game is not the lab. The golden vectors are the guard, and they exist only if S1 writes the dump format on day one.
4. `Generate.cpp` is shipped code edited for the sim's sake (S4). The unchanged-candidates pin is what keeps a light change from moving a release.
5. The ch.24 refactor (G2). It is the kind of "no behaviour change" move whose breakage shows only in the field. Log-line equality on a recorded fight is the bar, and it goes in before the pass leans on it.
6. The cost estimate stands on little. S5's 12-fight measurement decides between a five day round and a longer one.
7. The spot light sampler is a lead from one call site. If L1 fails, darkness is known only where an actor has stood. That is still useful, and the plan does not need more to ship the veto.
