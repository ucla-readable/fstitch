#include <inc/hash_map.h>
#include <inc/lib.h>

void umain(void)
{
	int r;

	hash_map_t * hm = hash_map_create();
	if (!hm)
		panic("hash_map_create FAILED");

	int ak = 1, av = 42;
	int bk = 2, bv = 69;

	// Test insert
	if ((r = hash_map_insert(hm, &ak, &av)) < 0)
		panic("hash_map_insert FAILED: %i", r);
	if ((r = hash_map_insert(hm, &bk, &bv)) < 0)
		panic("hash_map_insert FAILED: %i", r);

	// Check size is correct
	assert(hash_map_size(hm) == 2);

	// Test resizing
	if ((r = hash_map_resize(hm, 100)) < 0)
		panic("hash_map_resize FAILED: %i", r);
	assert(hash_map_size(hm) == 2);
	assert(hash_map_bucket_count(hm) >= 100);

	// Check finding works
	if (hash_map_find_val(hm, &ak) != &av)
		panic("hash_map_find FAILED");
	if (hash_map_find_val(hm, &bk) != &bv)
		panic("hash_map_find FAILED");

	// Check that erase works
	if (&bv != hash_map_erase(hm, &bk))
		panic("hash_map_erase FAILED");
	hash_map_elt_t hme_b = hash_map_find_elt(hm, &bk);
	if (hme_b.key || hme_b.val)
		panic("hash_map_find after erase FAILED");

	assert(hash_map_size(hm) == 1);

	// Check that find doesn't wrongly succeed
	hash_map_elt_t hme_av = hash_map_find_elt(hm, &av);
	if (hme_av.key || hme_av.val)
		panic("hash_map_find on a's val FAILED");

	// Check that change_key works
	int ak2 = 0;
	if ((r = hash_map_change_key(hm, &ak, &ak2)) < 0)
		panic("hash_map_change_key FAILED: %i", r);
	if (hash_map_find_val(hm, &ak2) != &av)
		panic("hash_map_find_val FAILED");
	if (hash_map_find_val(hm, &ak))
		panic("hash_map_find_val FAILED");

	// Check that clear works
	hash_map_clear(hm);

	assert(hash_map_size(hm) == 0);
	assert(hash_map_empty(hm));

	hash_map_destroy(hm);
	hm = NULL;
}
