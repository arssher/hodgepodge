`ext/` is usual Postgres extension (installed with `make install`), `go/` is Go
code (built with `make`, produces binaries in `go/bin/`), `devops/` contains Ansible
scripts and other stuff for deployment; `bin/` is some scripts for
testing.

There are two layers from configuration and administration point of view. The
first is *replication groups*. Shardman cluster consists of multiple replication
groups, each one being
separate [Stolon](https://github.com/sorintlab/stolon/) instance (cluster in
Stolon docs). Each replication group holds a piece of data, so you might call it
a shard. Stolon provides fault tolerance for each piece. Shardman shards tables
by partitioning them\: local tables are just usual partitions and remote tables
are `postgres\_fdw` foreign tables pointing to current master of replicaton group
holding the partition. General cluster management (repgroups addition, removal,
Postgres and Stolon conf) is done via `shardmanctl` binary; sharding tables is
done directly via SQL on any master.

Replication groups themselves don't know anything about physical location of
Postgres instances. When repgroup is being added to the cluster with
`shardmanctl`, it is assumed that Stolon instance forming it already
exists. Here comes the second layer, making this horribly complicated deploy
easier. With `shardman-ladle` you specify cluster conf and register physical
nodes: `ladle` computes which daemons (stolon keepers, sentinels, shardman
monitors) will run on on which nodes and their parameters. It pushes this info
to the store (`etcd`). On each node, `shardman-bowl` meta-daemon should be
started by the administrator. It reads info computed by `ladle` and spins off
specified daemons as systemd units. Once Stolon instances are up,
`shardman-ladle` registers them as new replication groups. Thus, generally you
should never need to manually add/remove replication groups.

Apart from infinite Stolon daemons, there is a stateless `shardman-monitor`
daemon which should always be running in the cluster -- or better at least
several of them (it is safe) for fault tolerance. `shardman-ladle` and `shardman-bowl`
can run it on configured number of nodes. Monitor
* Makes sure each repgroup is aware of location of current master of each
  other repgroup.
* Resolves 2PC (distributed) transactions.
* Resolves distributed deadlocks.

All golang binaries have `--help` with describing what commands they have,
and also `<binary> <command> --help` per-command help.

### Deployment

`devops/` dir contains Ansible scripts and templates of service files used by
`shardman-bowl` to start systemd units. Playbooks behaviour is influenced by
vars listed in `group_vars/all.yml` with their defaults. Playbooks operate on
`nodes` and `etcd_nodes` groups; `inventory_manual` has an example of simple
inventory.  `provision.yml` installs everything needed: etcd, postgres, stolon,
shardman. Also, during execution of etcd role, a etcd cluster is configured
(unless `etcd_service` skipped). `init.yml` inits single shardman cluster:
it instantiates all needed service files, starts `shardman-bowl` daemons and
executes `shardman-ladle`, creating cluster and adding `nodes` to it.

#### Example

Probably simplest way to test things is to fire up 4 nodes via `vagrant up`.
Inventory for them is listed in `inventory_manual.example`. Uncomment it, and then
```
cd devops/
pipenv install && pipenv shell # install and activate python env with installed ansible
ansible-playbook -i inventory_manual/ provision.yml
```
This installs etcd, configured etcd cluster on `etcd_nodes`, installs postgres,
stolon, shardman everywhere (on `nodes`).

Then
```
ansible-playbook -i inventory_manual/ init.yml
```
creates shardman cluster. Namely, it
* Instantiates systemd unit files on all nodes.
* Runs `shardman-bowl` daemons on all nodes.
* Executes on one random one `shardman-ladle init` to create cluster and `shardman-ladle addnodes`
  to register all nodes. Cluster specification is instantiated from `shmnspec.json.j2`;
  cluster name and path to data dir is taken from ansible vars (defaults in `group_vars/all.yml`)


### Using the cluster
All functions are in `shardman` schema.

Function
```
hash_shard_table(relid regclass, nparts int, colocate_with regclass = null) returns void
```

is used to hash shard table `relid` to `nparts` partitions. If `colocate_with`
is not null, partitions are placed to the same repgroups as partitions of
`colocate_with` table.

Table must present on all repgroups and be partitioned by hash. By default,
shardman broadcasts some utility statements to all repgroups automatically if
this makes sense and supported, including `CREATE
TABLE`. `shardman.broadcast_utility` GUC which can be set at any time controls
this behaviour: when `off`, no statements are broadcasted.

Example:
```
create table pt (id serial, payload real) partition by hash(id);
select shardman.hash_shard_table('pt', 10)
```

To execute a piece of SQL manually on all repgroups,
```
bcst_all_sql(cmd text) returns void
```
can be used.


New repgroups can be added at any time with `shardman-ladle addnodes`. Initially
they don't hold any data; to rebalance, use `shardmanctl rebalance`.
