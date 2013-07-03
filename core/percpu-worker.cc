#include <debug.hh>
#include <sched.hh>
#include <preempt-lock.hh>
#include <osv/trace.hh>
#include <osv/percpu.hh>
#include <osv/percpu-worker.hh>

TRACEPOINT(trace_pcpu_worker_started, "");
TRACEPOINT(trace_pcpu_worker_invoke, "item=%p", worker_item*);
TRACEPOINT(trace_pcpu_worker_sheriff, "num_items=%d", size_t);
TRACEPOINT(trace_pcpu_worker_signal, "item=%p, dest_cpu=%d, wait=%d", worker_item*, unsigned, bool);
TRACEPOINT(trace_pcpu_worker_wait, "item=%p", worker_item*);
TRACEPOINT(trace_pcpu_worker_end_wait, "item=%p", worker_item*);
TRACEPOINT(trace_pcpu_worker_set_finished, "item=%p, dest_cpu=%d", worker_item*, unsigned);

sched::cpu::notifier workman::_cpu_notifier(workman::pcpu_init);

PERCPU(std::atomic<bool>, workman::_duty);
PERCPU(std::atomic<bool>, workman::_ready);
PERCPU(sched::thread*, workman::_work_sheriff);

extern char _percpu_workers_start[];
extern char _percpu_workers_end[];

workman _workman;

worker_item::worker_item(std::function<void ()> handler)
{
    _handler = handler;
    for (unsigned i=0; i < sched::max_cpus; i++) {
        _have_work[i].store(false, std::memory_order_relaxed);
    }
}

void worker_item::signal(sched::cpu* cpu, bool wait)
{
    _have_work[cpu->id].store(true, std::memory_order_release);
    _workman.signal(cpu, this, wait);
}

void worker_item::set_finished(sched::cpu* cpu)
{
    trace_pcpu_worker_set_finished(this, cpu->id);
    _waiters[cpu->id].wake_all();
}

void worker_item::wait_for(unsigned cpu_id, sched::thread* wait_thread)
{
    _waiters[cpu_id].wait(nullptr, nullptr, wait_thread);
}

void workman::signal(sched::cpu* cpu, worker_item* item, bool wait)
{
    trace_pcpu_worker_signal(item, cpu->id, wait);

    if (!(*_ready).load(std::memory_order_relaxed)) {
        return;
    }

    //
    // let the sheriff know that he have to do what he have to do.
    // we simply set _duty=true and wake the sheriff
    //
    // when we signal a worker_item, we set 2 variables to true, the per
    // worker_item's per-cpu _have_work variable and the global _duty variable
    // of the cpu's sheriff we are signaling.
    //
    // why use std::atomic with release->acquire?
    //
    // we want the sheriff to see _duty=true only after _have_work=true.
    // in case duty=true will be seen before _have_work=true, we may miss
    // it in the sheriff thread.
    //
    (*(_duty.for_cpu(cpu))).store(true, std::memory_order_release);
    if (!wait) {
        (*_work_sheriff.for_cpu(cpu))->wake();
    } else {
        trace_pcpu_worker_wait(item);
        // Wait until the worker item finished
        item->wait_for(cpu->id, (*_work_sheriff.for_cpu(cpu)));
        trace_pcpu_worker_end_wait(item);
    }

}

void workman::call_of_duty(void)
{
    (*_ready).store(true, std::memory_order_relaxed);
    trace_pcpu_worker_started();

    while (true) {
        // Wait for duty
        sched::thread::wait_until([&] {
            return ((*_duty).load(std::memory_order_acquire) == true);
        });

        (*_duty).store(false, std::memory_order_release);

        unsigned cpu_id = sched::cpu::current()->id;

        // number of work items
        size_t num_work_items =
            (_percpu_workers_end-_percpu_workers_start) / sizeof(worker_item);
        trace_pcpu_worker_sheriff(num_work_items);

        // FIXME: we loop on the list so this is O(N), if the amount of PCPU
        // workers grow above 10-20 than maybe it's better to re-think this
        // design.
        std::lock_guard<preempt_lock_t> guard(preempt_lock);
        for (unsigned i=0; i < num_work_items; i++) {
            worker_item* it = reinterpret_cast<worker_item*>
                (_percpu_workers_start + i * sizeof(worker_item));

            // if the worker_item is signaled on our cpu, we will make sure
            // the handler does it's work.
            if (it->_have_work[cpu_id].load(std::memory_order_acquire)) {
                (it->_have_work[cpu_id]).store(false, std::memory_order_release);
                trace_pcpu_worker_invoke(it);
                it->_handler();
                it->set_finished(sched::cpus[cpu_id]);
            }
        }
    }
}

void workman::pcpu_init()
{
    // initialize the sheriff thread
    (*_duty).store(false, std::memory_order_relaxed);
    *_work_sheriff = new sched::thread([] { workman::call_of_duty(); },
        sched::thread::attr(sched::cpu::current()));
    (*_work_sheriff)->start();
}