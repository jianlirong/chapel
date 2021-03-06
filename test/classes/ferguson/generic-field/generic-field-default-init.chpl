pragma "use default init"
record GenericRecord {
  var field;
}

pragma "use default init"
class GenericClass {
  var f:GenericRecord;
}

proc test() {
  var x = new borrowed GenericClass(new GenericRecord(1));
  var y:borrowed GenericClass = new borrowed GenericClass(new GenericRecord(1));
  var z:borrowed GenericClass(GenericRecord(int)) = new borrowed GenericClass(new GenericRecord(1));

  writeln(x.type:string, " ", x);
  writeln(y.type:string, " ", y);
  writeln(z.type:string, " ", z);
  //var x:GenericClass; // no, not possible
}

test();
