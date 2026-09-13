# Workflows and delegation

Use a graph when work has dependencies or can run in parallel. Keep nodes
small: research, implementation, tests, and review stay separate. Bind a
predecessor's result when the next worker needs the exact findings. Add a
bounded feedback edge when a reviewer can send work back for another attempt.

Rules that actually matter:

- write acceptance criteria before execution
- give reviewers independence from implementers
- never treat a successful model response as proof that tests pass
- prefer bounded retries over infinite loops
- inspect authorization, persistence, and ownership before adding tools
