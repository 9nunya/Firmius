# Workflows and delegation

Use a graph when tasks have dependencies or can run in parallel. Keep nodes
small: research, implementation, tests, and review should be separate. Bind a
predecessor's result when the next worker needs its exact findings. Add a
bounded feedback edge when a reviewer can send work back for another attempt.

Good workflow rules:

- define acceptance criteria before execution;
- give reviewers independence from implementers;
- never treat a successful model response as proof that tests pass;
- prefer bounded retries over infinite agent loops;
- inspect authorization and persistence behavior for new tools.