import gdb


def value_to_int(value):
    return int(value.cast(gdb.lookup_type('uint32_t')).format_string(raw=True))


def set_reg(reg, value):
    gdb.execute('set ${} = {}'.format(reg, value))


class Stack(object):
    def __init__(self, pointer):
        # work with plain integer because the gdb value is immutable
        self._pointer = value_to_int(pointer)

    def pop(self):
        result = value_to_int(gdb.parse_and_eval('*{}'.format(self._pointer)))
        self._pointer += 4
        return result

    def pointer(self):
        return self._pointer


class Task(object):
    def __init__(self, tcb_ptr, *args, **kwargs):
        self._tcb_ptr = tcb_ptr
        self._status = kwargs['status']

    def number(self):
        return value_to_int(self._tcb_ptr['uxTaskNumber'])

    def name(self):
        s = ''
        task_name = self._tcb_ptr['pcTaskName']
        for i in range(task_name.type.sizeof):
            c = task_name[i]
            if c == 0:
                break
            s += chr(c)
        return s

    def is_running(self):
        return self._tcb_ptr == gdb.parse_and_eval('pxCurrentTCB')

    def status(self):
        return 'running' if self.is_running() else self._status

    def priority(self):
        return value_to_int(self._tcb_ptr['uxPriority'])

    def stack(self):
        return Stack(self._tcb_ptr['pxTopOfStack'])

    def gdb_name(self):
        return '{} (Thread <{}>)'.format(self.number(), self.name())


def foreach_freertos_list(freertos_list):
    index = freertos_list['pxIndex']
    count = freertos_list['uxNumberOfItems']
    while count:
        if index == freertos_list['xListEnd'].address:
            pass  # skip the marker used at the end of the list
        else:
            yield index.dereference()
            count -= 1

        index = index['pxNext']


def extract_tasks(task_list_cpp, *args, **kwargs):
    tcb_ptr_type = gdb.lookup_type('TCB_t').pointer()

    task_list = gdb.parse_and_eval(task_list_cpp)

    for list_item in foreach_freertos_list(task_list):
        tcb_ptr = list_item['pvOwner'].cast(tcb_ptr_type)
        yield Task(tcb_ptr, *args, **kwargs)


def collect_all_tasks():
    for task in extract_tasks('xDelayedTaskList1', status='blocked'):
        yield task
    for task in extract_tasks('xDelayedTaskList2', status='blocked'):
        yield task
    for i in range(0, 7):
        for task in extract_tasks('pxReadyTasksLists[{}]'.format(i),
                                  status='ready'):
            yield task


def switch_to_task(task):
    if task.is_running():
        # this would restore the task to the last context switch
        # instead of the current state, which is not what we want
        raise ValueError('Switching to running task')

    # Restore registers from the task's top of the stack
    # This basically reimplements xPortPendSVHandler()/PendSV_Handler()
    # and is immediately followed by MCU performing mode switch

    stack = task.stack()

    # r0-r3 will be restored by MCU performing mode switch

    # r4-r11 are restored by PendSV_Handler from the stack
    set_reg('r4', stack.pop())
    set_reg('r5', stack.pop())
    set_reg('r6', stack.pop())
    set_reg('r7', stack.pop())
    set_reg('r8', stack.pop())
    set_reg('r9', stack.pop())
    set_reg('r10', stack.pop())
    set_reg('r11', stack.pop())

    # r12 (scratch register) will be restored by MCU performing mode switch

    # r13 (sp) will be restored by MCU performing mode switch

    # r14 (lr) is restored by PendSV_Handler from the stack
    r14 = stack.pop()
    set_reg('r14', r14)

    # r15 (pc) will be restored by MCU performing mode switch

    # s16-s31 are restored by from stack if we are returning to fpu context
    is_fpu_context = bin(r14)[-5] == '0'
    if is_fpu_context:
        for i in range(16, 32):
            set_reg('s{}'.format(i), stack.pop())

    # psp will be used by MCU to restore sp when performing mode switch
    set_reg('psp', stack.pointer())

    # bx lr
    set_reg('pc', r14)

    # xpsr, fpscr and msp should not be touched


class FreeRTOS(gdb.Command):
    '''FreeRTOS gdb plugin.
    Available subcommands:

    freertos info threads
        - print information about all FreeRTOS tasks

    freertos thread
        - report current FreeRTOS task

    freertos thread [id]
        - switch to given FreeRTOS task

    freertos thread apply all [command...]
        - for each FreeRTOS task, switch to it and run given command
    '''

    def __init__(self):
        super(FreeRTOS, self).__init__('freertos', gdb.COMMAND_USER)
        self._save_state()

    def invoke(self, arg, from_tty):
        args = arg.strip().split()

        if not args:
            print(FreeRTOS.__doc__)
        elif args == ['info', 'threads']:
            self._info_threads()
        elif args[0:3] == ['thread', 'apply', 'all']:
            self._thread_apply_all(args[3:])
        elif args[0] == 'thread':
            self._thread(args[1:])
        else:
            print(FreeRTOS.__doc__)

        self.dont_repeat()

    def _info_threads(self):
        print('  Id Name             Status  Priority')
        print('  ------------------------------------')
        for task in sorted(collect_all_tasks(), key=lambda t: t.number()):
            task_id = str(task.number()).rjust(2)
            task_name = task.name().ljust(16)
            task_status = task.status().ljust(7)
            task_priority = str(task.priority()).rjust(8)
            print('  {} {} {} {}'.format(task_id, task_name, task_status,
                                         task_priority))

    def _thread_apply_all(self, args):
        if not args:
            return

        for task in sorted(collect_all_tasks(), key=lambda t: t.number()):
            print('\nThread {}:'.format(task.gdb_name()))
            self._switch_to_task(task)
            gdb.execute(' '.join(args))

    def _thread(self, args):
        if not args:
            task = Task(gdb.parse_and_eval('pxCurrentTCB'), status='running')
            print('[Current thread is {}]'.format(task.gdb_name()))
            return

        for task in collect_all_tasks():
            if str(task.number()) == args[0]:
                print('[Switching to thread {}]'.format(task.gdb_name()))
                self._switch_to_task(task)
                gdb.execute('frame')
                return

        print('Unknown thread {}.'.format(args[0]))

    def _switch_to_task(self, task):
        if task.is_running():
            self._restore_state()
        else:
            switch_to_task(task)

    def _save_state(self):
        self._saved_regs = {}

        def save_reg(name):
            value = gdb.parse_and_eval('${}'.format(name))
            self._saved_regs[name] = value_to_int(value)

        save_reg('r0')
        save_reg('r1')
        save_reg('r2')
        save_reg('r3')
        save_reg('r4')
        save_reg('r5')
        save_reg('r6')
        save_reg('r7')
        save_reg('r8')
        save_reg('r9')
        save_reg('r10')
        save_reg('r11')
        save_reg('r12')
        save_reg('r13')
        save_reg('r14')
        save_reg('r15')

    def _restore_state(self):
        for name, value in self._saved_regs.items():
            set_reg(name, value)


# instantiation is necessary to trigger registration of command with gdb
FreeRTOS()
