#undef TRACE_SYSTEM
#define TRACE_SYSTEM raw_syscalls

#if !defined(_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_H_

#include <linux/tracepoint.h>

/*
 * The TRACE_EVENT macro is used to define the tracepoints.
 * We are not defining new events, but we need to include the file
 * that does, and this is the standard way to do it.
 * The kernel build system will find the correct header
 * (include/trace/events/raw_syscalls.h) for us.
 */
TRACE_EVENT(raw_syscalls_sys_enter,
    TP_PROTO(struct pt_regs *regs, long id),
    TP_ARGS(regs, id)
);

TRACE_EVENT(raw_syscalls_sys_exit,
    TP_PROTO(struct pt_regs *regs, long ret),
    TP_ARGS(regs, ret)
);

#endif /* _TRACE_H_ */

/*
 * This must be included outside the header guard,
 * to allow for multiple inclusions of this file.
 */
#include <trace/define_trace.h>