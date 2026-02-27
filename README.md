# Walden

> I see young men, my townsmen, whose misfortune it is to have inherited farms, houses, barns, cattle, and farming tools; for these are more easily acquired than got rid of. Better if they had been born in the open pasture and suckled by a wolf, that they might have seen with clearer eyes what field they were called to labor in.

&mdash; Henry David Thoreau, *Walden*

## What is this?

The goal here is to build a **Plan 9**-*inspired*, *non-POSIX*, *single-user*, *microkernel-based*, *LoongArch64-only* opinionated operating system.

This is not corporate. There is no product, there is no "target market," I make no promises, and I **certainly** don't know what I'm doing.

### Single-user?

Our security model is "protect the user from processes & processes from each other," not "protect the user from other users."

I feel that the "multi-user" concept is an archaic abstraction we inherited from 1970s time-sharing systems, and that most modern devices are *in fact* single-user systems. We then shoehorn contemporary security models on top of this ill-fitting "multi-user" abstraction (e.g., process daemons running as special "users" for least-privilege, even though those "users" don't represent real people).

### Plan 9-inspired?

I feel that *namespaces* are a good starting point to develop an alternative security model if we're getting rid of "users." Plus, I just think it's neat.

### Microkernel-based?

I don't know, it seems neat.

### LoongArch64-only?

Stop asking me these questions.

## License

This project is free software licensed under the GNU General Public License v3.0 or later (GPL-3.0+). See [LICENSE](LICENSE) for details.
