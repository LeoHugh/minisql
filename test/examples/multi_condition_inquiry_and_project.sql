use db0;
select id, name from account where balance >= 20000 and balance < 70000;
select name, balance from account where balance > 19999 and id <= 69999;
select * from account where id < 12515000 and name > "name14500";
select * from account where id < 12500200 and balance < 100;