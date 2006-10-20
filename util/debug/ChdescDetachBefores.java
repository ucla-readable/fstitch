public class ChdescDetachBefores extends Opcode
{
	private final int chdesc;
	
	public ChdescDetachBefores(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_DETACH_BEFORES: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DETACH_BEFORES, "KDB_CHDESC_DETACH_BEFORES", ChdescDetachBefores.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
