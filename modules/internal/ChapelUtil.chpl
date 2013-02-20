// ChapelUtil.chpl
//
// Internal data structures module
//
pragma "no use ChapelStandard"
module ChapelUtil {
  
  param _INIT_STACK_SIZE = 8;
  
  class _stack {
    type eltType;
    var  size: int;
    var  top: int;
    var  data: _ddata(eltType);
    
    proc initialize() {
      top = 0;
      size = _INIT_STACK_SIZE;
      data = new _ddata(eltType);
      data.init(8);
    }
  
    proc push( e: eltType) {
      if (top == size-1) {  // supersize as necessary
        size *= 2;
        var supersize = new _ddata(eltType);
        supersize.init(size);
        [i in 0..(size/2)-1] supersize[i] = data[i];
        data = supersize;
      }
      data[top] = e;
      top += 1;
    }
  
    proc pop() {
      var e: eltType;
      if top>0 then {
        top -= 1;
        e = data[top];
      } else {
        halt( "pop() on empty stack");
      }
      return e;
    }
  
    proc empty() {
      top = 0;
    }
  
    proc length {
      return top;
    }
  
    proc writeThis(f: Writer) {
      for i in 0..top-1 do f.write(" ", data[i]);
    }
  }
  
  //
  // safeAdd: If a and b are of type t, return true iff no
  //  overflow/underflow would occur for a + b
  //
  proc safeAdd(a: ?t, b: t) {
    if !_isIntegralType(t) then
      compilerError("Values must be of integral type.");
    if a < 0 {
      if b >= 0 {
        return true;
      } else {
        if b < min(t) - a {
          return false;
        } else {
          return true;
        }
      }
    } else {
      if b <= 0 {
        return true;
      } else {
        if b > max(t) - a {
          return false;
        } else {
          return true;
        }
      }
    }
  }
  
  //
  // safeSub: If a and b are of type t, return true iff no
  //  underflow/overflow would occur for a - b
  //
  proc safeSub(a: ?t, b: t) {
    if !_isIntegralType(t) then
      compilerError("Values must be of integral type.");
    if a < 0 {
      if b <= 0 {
        return true;
      } else {
        // This assumes min(t) = -max(t)+1 (as per the Chapel spec)
        // The reason you need to do this is b/c what you really want to say
        //  is b > abs(min(t)-a), but you can't be guaranteed tha abs()
        //  results is in range of t
        if b > max(t) + a {
          if safeAdd(max(t)+a, 1) {
            if b > max(t) + a + 1 {
              return false;
            } else {
              return true;
            }
          }
          return false;
        } else {
          return true;
        }
      }
    } else {
      if b >= 0 {
        if _isUnsignedType(t) then
          if b>a then return false;
        return true;
      } else {
        if b <= min(t) + a {
          return false;
        } else {
          return true;
        }
      }
    }
  }
  
  extern record chpl_main_argument {
    var argc: int(64);
    var argv: _ddata(string);
    var return_value: int(32);
  }

  proc chpl_convert_args(arg: chpl_main_argument) {
    var local_arg = arg;
    extern proc chpl_get_argument_i(ref args:chpl_main_argument, i:int(32)):string;
    // This is odd.  Why are the strings inside the array getting destroyed?
    pragma "no auto destroy"
    var array: [0..#local_arg.argc] string;

    for i in 0..#arg.argc {
      array[i] = chpl_get_argument_i(local_arg, i:int(32));
    }

    return array;
  }

}

