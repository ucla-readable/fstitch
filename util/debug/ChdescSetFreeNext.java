public class ChdescSetFreeNext extends Opcode
{
	private final int chdesc, free_next;
	
	public ChdescSetFreeNext(int chdesc, int free_next)
	{
		this.chdesc = chdesc;
		this.free_next = free_next;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
		{
			if(free_next == 0)
				chdesc.setFreeNext(null);
			else
			{
				Chdesc free_next = state.lookupChdesc(this.free_next);
				if(free_next == null)
				{
					free_next = new Chdesc(this.free_next, state.getOpcodeNumber());
					/* should we really do this? */
					state.addChdesc(free_next);
				}
				chdesc.setFreeNext(free_next);
			}
		}
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_FREE_NEXT: chdesc = " + SystemState.hex(chdesc) + ", free_next = " + SystemState.hex(free_next);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_FREE_NEXT, "KDB_CHDESC_SET_FREE_NEXT", ChdescSetFreeNext.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("free_next", 4);
		return factory;
	}
}
