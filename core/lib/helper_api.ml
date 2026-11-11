type value = Vint of int64 | Vfloat of float
type pred = Eq of int64 | Range of float * float | Lt_prev | Gt_prev | Eq_prev | Ne_prev
type group_spec = { id : string; vtype : string; align : int; exclude_align : int; pred : pred }
type group_info = { id : string; count : int; tracked : bool; has_values : bool }
type stats = { regions : int; bytes : int; unreadable_bytes : int; lost : int; ms : float }
type item = { addr : int64; v : value }
type attached = { pid : int; name : string; bits : int }
type err = { code : string; message : string }

(* what a write puts in memory: a value, or a value's raw bytes (little-endian, in the low bits) *)
type put = Put_value of value | Put_bits of int64

(* what a write replaced: the value, and its raw bytes *)
type replaced = { prev : value; prev_bits : int64 }

type helper = {
  ps : unit -> ((int * string) list, err) result;
  modules : int -> (string list, err) result;
  attach : int -> (attached, err) result;
  scan : keep:bool -> group_spec list -> (group_info list * stats, err) result;
  narrow : atomic:bool -> (string * pred) list -> (bool * group_info list * stats, err) result;
  drop : string list -> (unit, err) result;
  peek : id:string -> vtype:string -> offset:int -> limit:int -> (item list, err) result;
  read : addr:int64 -> vtype:string -> (value, err) result;
  write : addr:int64 -> vtype:string -> put -> (replaced, err) result;
  status : unit -> (attached option, err) result; (* None: not attached, or the process has exited *)
  detach : unit -> (unit, err) result;
  close : unit -> unit;
}
