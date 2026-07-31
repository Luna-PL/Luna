# Loop-Local Slot Plugins

This example demonstrates a type-safe runtime plugin injection point:

~~~text
dynamic slot context hook(value: i32);
dynamic apply hook(log, audit) { ... }
~~~
The hook is inside the while-loop body and is invoked again on every iteration.
log and audit are statically linked candidate contexts; the runtime selects one
through an environment variable:

~~~sh
LUNA_GPU_BACKEND=sim ./build/luna run \
  examples/slot_plugins/loop_plugins.luna

LUNA_GPU_BACKEND=sim LUNA_FRAGMENT_HOOK=audit \
  ./build/luna run examples/slot_plugins/loop_plugins.luna
~~~
