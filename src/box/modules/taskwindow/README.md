# TaskWindow

A native C version of the RISC OS 5 TaskWindow module (`Desktop/TaskWindow`, 0.85). A task window runs a command as a Wimp task of its own. What the command prints is sent to the task window's parent as Wimp messages, and what it reads comes from the parent. The parent is usually an editor such as !Edit, or any task that starts a task window with itself as the parent. The farm's job runner does this.

## Commands and SWI

| Command | Meaning |
| --- | --- |
| `*TaskWindow "<command>" [options]` | Enters the module as the application: a new task window. |
| `*ShellCLI_Task` | The old form. Two 8-digit hex numbers (parent task and text handle) and no command. |
| `*ShellCLI_TaskQuit` | In a task window, ends it after this command. |

Options: `-wimpslot <n>K`, `-name <name>`, `-display` (ask for a parent at once), `-quit` (end when the command ends), `-ctrl` (pass control characters), `-task <n>`, `-txt <n>`, `-nice <n>` (centiseconds in a time slice, default 10) and `-vdisplay <n>`.

`-vdisplay` is specific to the BOX. It attaches a virtual display (`VDisplay_Attach`) so that the VDU draws the program's output into it, and the parent is sent no Output. GraphTask uses it.

`TaskWindow_TaskInfo` (&43380, `api/defs/taskwindow.toml`) returns non-zero in a task window's program and 0 elsewhere.

## Messages

User messages &808C0 to &808C7: Input, Output (at most 231 bytes), Ego, Morio, Morite, NewTask, Suspend and Resume. Output goes through a ring of 232 bytes. It is sent when full, before each poll, and every tenth poll. A task window with no parent that needs one broadcasts NewTask. If nobody answers and `TaskWindow$Server` is set, it runs that. Otherwise the error "Task window cannot be opened" (&A85) results.

Without `-ctrl`, control characters other than LF are dropped, with the bytes of their VDU sequences.

Input comes from the redirection file, then the `*Exec` file, then the key buffer. The real escape key is off while the program runs, and the task window emulates it.

## Time slices

TickerV counts the centiseconds the program has run. After `-nice` of them a CallBack sends the output and polls the Wimp. Each task is a thread with its own SVC stack, and Wimp_Poll is made as the outermost SWI so that its way out switches tasks. A task window also yields when waiting for input, and in OS_UpCall 6 (UpCall_Sleep), which lets native commands such as `*Ping` and `*SSH` wait without blocking the desktop. UpCall_SleepNoMore on a word a task window sleeps on gives &A84.

At initialisation it sets `Alias$@RunType_FD6` (TaskExec) and `Alias$@RunType_FD7` (TaskObey) if they are not set. Service_Memory is claimed while it starts. Service_WimpCloseDown for a task window's task is refused with &104. Service_WimpReportError releases the vectors while an error box is open.

Errors: &A80 task window still active (on finalising), &A82 bad task or text handle, &A83 Task dying, &A84, &A85, &104 and &1E6.

## Differences from RISC OS 5.30

- There is no pre-emption in the BOX. RISC OS 5.30 pre-empts a program that never calls the OS. Here such a program yields at its next SWI exit. Probe `preempt` records the difference and is expected to fail.
- RISC OS 5.30 takes any task window with text handle 1 as the one waiting for a parent. Here it must also have no parent.
- `*TaskWindow` with no parameters returns its syntax error. On 5.30 XOS_CLI raises it instead.

## Not done

- A separate command line for each task. The kernel's one string is shared. RISC OS replaces OS_GetEnv and OS_WriteEnv for this.
- Pasting by DataSave, RAMFetch and RAMTransmit. A recorded DataSave is acknowledged but the data is not fetched.
- A pending OS_SetCallBack of the program's own, saved across a poll.

Tests: `tests/desktop/taskwindow` holds probes compared with RISC OS 5.30 on the farm (`ctrl`, `timeslice`, `noslice` and `preempt` among them). `boot/selftest_taskwindow.c` checks the rest.

## Licence

Each source file names its licence in its header.
The BOX project asserts no ownership of translated code.
