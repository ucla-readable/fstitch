public class ChdescSetFreeHead extends Opcode
{
	private final int chdesc;
	
	public ChdescSetFreeHead(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
		if(chdesc == 0)
			state.setFreeHead(null);
		else
		{
			Chdesc chdesc = state.lookupChdesc(this.chdesc);
			if(chdesc == null)
			{
				chdesc = new Chdesc(this.chdesc, state.getOpcodeNumber());
				/* should we really do this? */
				state.addChdesc(chdesc);
			}
			state.setFreeHead(chdesc);
		}
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_FREE_HEAD: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_FREE_HEAD, "KDB_CHDESC_SET_FREE_HEAD", ChdescSetFreeHead.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
