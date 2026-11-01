# Workflows

!Workflows makes workflows: Tcl scripts (type &A6C) that take documents through stages. A workflow is shown as a page made from it, with its inputs and outputs, its stages drawn as a flowchart and its tests, and its Tcl is edited in the source view. Run runs it in a task window and lights up each stage as it runs; Test runs its own tests; Record hears what is done in the other applications and puts it into the workflow as stages. It uses !Draft's engine.

`Workflows.mojo` is the application. It needs the shared shell, `../shell/rosapp.mojo`.

## Licence

MIT. Each source file names its licence in its header.
