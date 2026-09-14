# Result Analysis Context

The scheduler and plugins execute tasks and persist raw outcomes. The result
service reads durable run metadata, file outcomes, and plugin-owned derived
results for display and analysis.

`finished` means execution completed. It does not mean every file succeeded
or that post-processing completed. Keep execution status, result completeness,
and analysis status separate in answers.

The assistant receives a structured result context selected by the operator.
It must not request database credentials, issue arbitrary SQL, modify results,
or claim that a run is complete without evidence in the context.

