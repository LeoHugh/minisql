use db0;
create table account(
    id int,
    name char(16) unique,
    balance float,
    primary key(id)
);