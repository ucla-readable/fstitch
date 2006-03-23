public class ChdescWeakCollect extends Opcode
{
	private final int chdesc;
	
	public ChdescWeakCollect(int chdesc)
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
		return "KDB_CHDESC_WEAK_COLLECT: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_WEAK_COLLECT, "KDB_CHDESC_WEAK_COLLECT", ChdescWeakCollect.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
