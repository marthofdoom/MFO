# marth's voice — writing style guide

Purpose: write MFO's public-facing text (Nexus description, changelogs, announcements,
release notes, any copy that goes out under marth's name) in **marth's actual voice**, not
generic/AI marketing copy. Derived from marth's own writing and his direct corrections.
Read this BEFORE drafting any such text, and do an editing pass against it before handing
copy back.

## Hard rules — the AI tells, delete on sight

- **NO em-dashes ( — ). NO en-dashes ( – ). Ever.** marth: "I dont think ive ever used an em
  dash in my life." Use a period, a comma, a colon, or parentheses instead. Number ranges:
  "300 to 600 ms", not "300–600".
- **NO semicolons.** Break into two sentences, or use a comma / "and".
- **No reflexive rule-of-three.** Balanced triads ("it does X, it does Y, and it does Z"),
  parallel-clause symmetry, and tidy marketing cadence read as generated. Let sentences be
  uneven.
- **No flourish closers** — a sentence that exists only to sound good ("instead of swinging at
  empty air", "the way it was always meant to be"). Cut it or make it plain.

## Voice

- **Short, direct, declarative.** State the thing, then explain it plainly. Feature = a bold
  phrase, a period, then plain sentences.
- **Confident and matter-of-fact** about what the mod does, and **honest about limits** in the
  same breath ("This is an extremely complicated mod, there are likely still some issues I
  haven't found").
- **Plain over polished.** Technical precision, no jargon for its own sake, no salesmanship.
- **Parentheses for asides and caveats:** "(your pick)", "(and that's ok)", "(off by default)".
- **Concrete examples carry the point:** "an archer won't swap to a mace", "leave someone in
  Breezehome for two hundred hours".
- **Direct address:** "your pick", "You never see a number. You read it through behaviour."

## Mechanics

- **Published text is CLEAN and uses PROPER punctuation and contraction etiquette** even though
  marth's chat messages are lowercase, apostrophe-dropped, and typo'd. Correct spelling,
  capitalization, and grammar; apostrophes in contractions (don't, they're, it's, won't, that's).
  marth: "No typos though" and "I would normally use proper punctuation and contraction
  etiquette, at least including single quotes in things like don't. But I don't always do that in
  our conversations." Do NOT mirror the casual chat style into published copy.
- BBCode: `[b]` for feature names / key toggles / MCM keys, `[i]` for in-game terms and emphasis
  words, `[list]`/`[*]` for enumerations.

## Before/after (real edit)

- DON'T (AI): "Concentration heals actually channel — they heal a target to full and stop,
  break off the moment the caster runs dry, and never spam or stall."
- DO (marth): "Heals channel the way they should now. They top a target off and stop, they quit
  the moment the caster is out of magicka, and they don't spam or drain him dry."

## How marth actually builds sentences (collated from the full conversation)

- **Cause and effect with "so".** State the fact, then the consequence. "He's a 1h and he's 2h
  so that's obvious." "A follower cannot heal and attack in the same cycle, so the cost of a heal
  is the attack not made." This is the single most characteristic move. Use it.
- **Starts sentences with And / But / So** when it flows. That's fine and natural here.
- **Blunt and decisive.** Flat statements and short imperatives. "Drop the handing entirely."
  "It's done." "No, we solved it too." No hedging, no throat-clearing.
- **Reasons out loud, plainly.** Lay out the situation, then the conclusion. "Logically we know
  the freeze was caused by the newer updates, so it's either the timing caps or the live spell
  alterations." Give the reader the reasoning, not just the claim.
- **Concrete and specific over abstract.** Name the thing (a mace, plate, Breezehome, a 1h vs a
  2h). Examples do the persuading.
- **Emphasis comes from word choice and structure, not adjectives.** Avoid the booster words that
  read as marketing or AI: "reliably", "seamlessly", "powerful", "robust", "smart", "actually",
  "simply", "truly". If a claim needs an adjective to land, it's usually the wrong claim.
- **Talks to the reader.** Direct "you", occasional "we". "Your pick." "You never see a number.
  You read it through behaviour."
- **Confident about what works, honest about what doesn't, in the same plain register.** No
  overselling. State a limit as flatly as a feature.
- Rhetorical questions are fine, especially as FAQ headers ("Why won't my follower cast the spell
  I set?").

## Process

1. Draft the content.
2. Search-and-destroy pass: delete every em-dash, en-dash, and semicolon; rewrite that clause.
3. Read it back for marketing cadence / balanced triads / flourish closers and break them.
4. Spell/grammar clean (published text is polished even if the source notes weren't).

Keep this doc updated as marth gives more voice corrections.

## Talking to another modder (dev-to-dev, 2026-09-22)

A different register from Nexus copy: it is a chat message, not published text, so the hard
rules still hold (no dashes, no semicolons) but the polish drops. Sample marth actually sent,
diagnosing a third-party plugin built on APMF:

> "Hmm, ok. I see the issue. You are successfully setting the combat target, but nothing is
> setting combat itself or handing a combat package. It looks like you are just moving the actor
> toward the target. I don't yet have a engage combat intent because normally you'd expect the
> games regular combat package to kick in. But I can add one pretty quick I think. I think one
> will be needed for certain spells anyway. Lemme keep digging for a little bit."

What that shows, and what to copy:
- **Think out loud in order.** "Hmm, ok. I see the issue." Lead with the read, not a preamble
  and not a summary of what they said.
- **Name what they DID right first,** then the missing piece, in the same breath: "You are
  successfully setting the combat target, but nothing is setting combat itself."
- **Own the gap without apology or ceremony.** "I don't yet have a engage combat intent." No
  "unfortunately", no "I'm sorry to say".
- **Explain why the gap was reasonable,** in one clause: "normally you'd expect the games
  regular combat package to kick in."
- **Hedge estimates honestly.** "pretty quick I think", "I think one will be needed", "Lemme
  keep digging for a little bit." Never promise a date or a release.
- **Short paragraphs, no headers, no bullet lists** unless the other person asked for steps.
  A numbered recipe is fine when it IS the deliverable, but keep it tight and skip the
  restatement of their own code back at them.
- **No lecture.** State the engine fact once ("StartCombat does not path anyone to a target they
  have not detected") and move on. Do not stack every finding in the report into the message,
  pick the ones that change what they would do next.
- Contractions stay natural ("you'd", "don't"). Casual words are fine ("Lemme", "pretty quick").
