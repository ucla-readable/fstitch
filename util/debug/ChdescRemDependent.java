public class ChdescRemDependent extends Opcode
{
	private final int source, target;
	
	public ChdescRemDependent(int source, int target)
	{
		this.source = source;
		this.target = target;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc source = state.lookupChdesc(this.source);
		if(source != null)
			source.remDependent(target);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_REM_DEPENDENT: source = " + SystemState.hex(source) + ", target = " + SystemState.hex(target);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_REM_DEPENDENT, "KDB_CHDESC_REM_DEPENDENT", ChdescRemDependent.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
