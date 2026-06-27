# Code Snippets: deterministic-spec

> ## SUPERSEDED -- Not Part of Current Implementation
>
> The initial PoC explored three error resolution strategies (penalize+resample,
> diagnostic injection, fix injection). These were **never implemented in
> production**. The current architecture uses truncate + reverify: the draft is
> truncated at the first structural error, the valid prefix is passed to the
> main model for one-sweep verification, and the model resumes naturally from
> the error point. This mirrors how MTP rejection already works in llama.cpp.
>
> This file is retained only as historical reference.
>
> Superseded: 2026-06-24
