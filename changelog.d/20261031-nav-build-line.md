---
area: bots
type: added
audience: admins
title: Bot movement logs name the build and count the bots online
---
The worldserver logs one `coa build` line at start with the build's sha, its commit subject and a hash of the `AiPlayerbot.Coa*` settings, and the `coa movement` line now starts with the number of bots online. The new option `AiPlayerbot.CoaBuildInfoFile` names the file that holds the sha and subject.
