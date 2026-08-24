# Memory and context

Firmius separates durable session state, compacted context, artifacts, and
transient worker state. Context budgets and compaction projections reduce the
amount of history sent to a provider; artifacts keep large outputs out of the
working prompt. This is the foundation for a serious memory story.

Memory efficiency depends on workload and provider. Do not compare screenshots
or anecdotal “tokens saved” numbers. Use the repository benchmark plan and
publish peak RSS, allocation behavior, latency, retained context, hardware,
versions, and raw results.