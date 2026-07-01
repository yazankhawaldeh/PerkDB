# PerkDB

## Description
This is a rudimentary CRUD API to write to a single file SQLite amalgamation database. This offers exceptional performance for small projects, and the API is written in C for super fast writes. This can be an alternative to PostgreSQL or other databases.
## What can this be used for?
This can be used for pretty much anything that requires a database and needs an easy way to interact with it. This project allows for super fast database handling, especially for embedded targets since the size is small and its a compiled binary. This is also good for small to medium scale web services and cuts out a need for a more complex databasde management system.
## What is the current state of the project?
Very bare bones, and there is only one person working on this project, but progress is coming slowly.
## What are the future plans / what needs to be done?
- Finish API functinality (Will be CRUD)
- Ensure TypeScript/JavaScript functinoality for web servers
- Structure the writing and reading of the API
## Why even make this when PostgreSQL and others exist?
The simplicity of it is what the point is. No external server handling traffic and writes, no special account or API keys needed. Just pure compiled C and SQLite database management. Single folder drop in and you're done. 
## What does the name mean
My nickname from my friends is Perky. 

