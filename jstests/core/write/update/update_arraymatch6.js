var res;
let t = db.jstests_update_arraymatch6;
t.drop();

function doTest() {
    t.save({a: [{id: 1, x: [5, 6, 7]}, {id: 2, x: [8, 9, 10]}]});
    res = t.update({'a.id': 1}, {$set: {'a.$.x': [1, 1, 1]}});
    assert.commandWorked(res);
    assert.eq(1, t.findOne().a[0].x[0]);
}

doTest();
t.drop();
t.createIndex({'a.id': 1});
doTest();
