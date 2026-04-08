Read STATUS.md and perform a consistency check. Do the following:

1. Print the current contents of STATUS.md verbatim.

2. For every file listed in Component Status:
   - If status is "done": verify the file exists on disk. If it is missing, flag it.
   - If status is "pending": confirm it is not yet present (or is a stub).

3. Check that the "Next Step" entry matches the lowest-numbered pending item in Phase Progress.

4. If any source files (.c, .h, .hip) exist: verify STATUS.md API Surface section lists their exported functions with exact signatures. If signatures are missing or stale, say so.

5. Report discrepancies as a compact list. If everything is consistent, say "STATUS consistent."

Do not modify STATUS.md during this command — only report findings.
