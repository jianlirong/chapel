
record Wrapper {
  type t;
  var Array:[1..1] t;
  delegate Array[1];
  proc foo() { writeln("in Wrapper.foo()"); }
}

class C {
  var field:int;
  proc foo() { writeln("in C.foo()"); }
  proc bar() { writeln("in C.bar()"); }
  proc baz() { field = 1; }
}

var r:Wrapper(C);
r.Array[1] = new C();
r.foo(); // prints "in Wrapper.foo()"
r.bar(); // same as r.instance.bar(), prints "in C.foo()"


