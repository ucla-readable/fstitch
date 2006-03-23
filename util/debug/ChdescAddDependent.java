public class ChdescAddDependent extends Opcode
{
	private final int source, target;
	
	public ChdescAddDependent(int source, int target)
	{
		this.source = source;
		this.target = target;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc source = state.lookupChdesc(this.source);
		if(source != null)
		{
			Chdesc target = state.lookupChdesc(this.target);
			if(target == null)
			{
				target = new Chdesc(this.target);
				/* should we really do this? */
				state.addChdesc(target);
			}
			source.addDependent(target);
		}
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ADD_DEPENDENT: source = " + SystemState.hex(source) + ", target = " + SystemState.hex(target);
	}
	
	public static ModuleOpcodeFactory getFactory(CountingDataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ADD_DEPENDENT, "KDB_CHDESC_ADD_DEPENDENT", ChdescAddDependent.class);
		factory.addParameter("source", 4);
		factory.addParameter("target", 4);
		return factory;
	}
}
