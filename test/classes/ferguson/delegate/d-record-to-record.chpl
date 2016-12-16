
record Wrapper {
  var instance; // e.g. some class
  delegate instance;
  proc foo() { writeln("in Wrapper.foo()"); }
}

record C {
  var field:int;
  proc foo() { writeln("in C.foo()"); }
  proc bar() { writeln("in C.bar()"); }
  proc baz() { field = 1; }
}

var r = new Wrapper(new C());
r.foo(); // direct method shadows delegated method
r.bar(); // same as r.instance.bar(), prints "in C.foo()"


