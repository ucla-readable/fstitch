public class ChdescClearFlags extends Opcode
{
	private final int chdesc, flags;
	
	public ChdescClearFlags(int chdesc, int flags)
	{
		this.chdesc = chdesc;
		this.flags = flags;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.clearFlags(flags);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_CLEAR_FLAGS: chdesc = " + SystemState.hex(chdesc) + ", flags = " + Chdesc.renderFlags(flags);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CLEAR_FLAGS, "KDB_CHDESC_CLEAR_FLAGS", ChdescClearFlags.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("flags", 4);
		return factory;
	}
}
