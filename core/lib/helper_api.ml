type value = Vint of int64 | Vfloat of float
type pred = Eq of int64 | Range of float * float | Lt_prev | Gt_prev | Eq_prev | Ne_prev
type group_spec = { id : string; vtype : string; align : int; exclude_align : int; pred : pred }
type group_info = { id : string; count : int; tracked : bool; has_values : bool }
type stats = { regions : int; bytes : int; unreadable_bytes : int; lost : int; ms : float }
type item = { addr : int64; v : value }
type attached = { pid : int; name : string; bits : int }
type err = { code : string; message : string }
type helper = {
  ps : unit -> ((int * string) list, err) result;
  modules : int -> (string list, err) result;
  attach : int -> (attached, err) result;
  scan : keep:bool -> group_spec list -> (group_info list * stats, err) result;
  narrow : atomic:bool -> (string * pred) list -> (bool * group_info list * stats, err) result;
  drop : string list -> (unit, err) result;
  peek : id:string -> offset:int -> limit:int -> (item list, err) result;
  read : addr:int64 -> vtype:string -> (value, err) result;
  write : addr:int64 -> vtype:string -> value -> (value, err) result;
  detach : unit -> (unit, err) result;
  close : unit -> unit;
}
