public class ChdescWeakForget extends Opcode
{
	private final int chdesc, location;
	
	public ChdescWeakForget(int chdesc, int location)
	{
		this.chdesc = chdesc;
		this.location = location;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.weakForget(location);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_WEAK_FORGET: chdesc = " + SystemState.hex(chdesc) + ", location = " + SystemState.hex(location);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_WEAK_FORGET, "KDB_CHDESC_WEAK_FORGET", ChdescWeakForget.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("location", 4);
		return factory;
	}
}
